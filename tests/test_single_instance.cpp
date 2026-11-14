// SPDX-License-Identifier: GPL-3.0
// 单实例获取 / 宿主重载接管逻辑测试。
//
// 使用带 PID + 序号的专属互斥体名，避免与正在运行的真实插件互相干扰。
#include <catch2/catch_all.hpp>

#include "core/single_instance.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
    // --hold <mutexName> <holdMs> <readyEventName>：
    // 供跨进程用例启动的「旧实例」。它占用单实例名字、置位 ready 事件、
    // 保持一段时间后直接退出，且不显式 CloseHandle —— 由内核在进程退出时
    // 关闭句柄并销毁命名对象，与真实插件进程完全一致。
    if (argc >= 5 && std::string(argv[1]) == "--hold") {
        const std::string mutexAscii = argv[2];
        const DWORD holdMs = static_cast<DWORD>(std::stoul(argv[3]));
        const std::string eventAscii = argv[4];
        const std::wstring mutexName(mutexAscii.begin(), mutexAscii.end());
        const std::wstring eventName(eventAscii.begin(), eventAscii.end());

        moekoe::InstanceLockOptions opt;
        opt.mutexName = mutexName.c_str();
        opt.managed = false;
        opt.timeoutMs = 0;
        const auto lock = moekoe::AcquireInstanceLock(opt);
        if (lock.result != moekoe::InstanceAcquireResult::Acquired) return 2;

        HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
        if (ready) {
            ::SetEvent(ready);
            ::CloseHandle(ready);
        }
        ::Sleep(holdMs);
        return 0;
    }
    return Catch::Session().run(argc, argv);
}

namespace {

std::string UniqueMutexNameNarrow() {
    static std::atomic<int> seq{0};
    return std::string("MoeKoeTaskbarLyrics_Test_") +
           std::to_string(static_cast<unsigned long>(::GetCurrentProcessId())) + "_" +
           std::to_string(seq.fetch_add(1));
}

std::wstring UniqueMutexName() {
    const auto narrow = UniqueMutexNameNarrow();
    return std::wstring(narrow.begin(), narrow.end());
}

moekoe::InstanceLockOptions Options(const std::wstring& name, bool managed,
                                    DWORD timeoutMs, DWORD pollMs = 20) {
    moekoe::InstanceLockOptions opt;
    opt.mutexName = name.c_str();
    opt.managed = managed;
    opt.timeoutMs = timeoutMs;
    opt.pollMs = pollMs;
    return opt;
}

} // namespace

TEST_CASE("First launch acquires the mutex; name is free again after release",
          "[single-instance]") {
    const auto name = UniqueMutexName();
    const auto opt = Options(name, false, 0);

    const auto first = moekoe::AcquireInstanceLock(opt);
    REQUIRE(first.result == moekoe::InstanceAcquireResult::Acquired);
    REQUIRE(first.handle != nullptr);
    REQUIRE_FALSE(first.timedOut);
    REQUIRE(first.waitedMs < 50u);
    moekoe::ReleaseInstanceLock(first.handle);

    const auto second = moekoe::AcquireInstanceLock(opt);
    REQUIRE(second.result == moekoe::InstanceAcquireResult::Acquired);
    REQUIRE(second.handle != nullptr);
    moekoe::ReleaseInstanceLock(second.handle);
}

TEST_CASE("Standalone duplicate exits immediately without waiting",
          "[single-instance]") {
    const auto name = UniqueMutexName();
    const auto opt = Options(name, false, 0);

    const auto holder = moekoe::AcquireInstanceLock(opt);
    REQUIRE(holder.result == moekoe::InstanceAcquireResult::Acquired);

    const auto duplicate = moekoe::AcquireInstanceLock(opt);
    REQUIRE(duplicate.result == moekoe::InstanceAcquireResult::DuplicateStandalone);
    REQUIRE(duplicate.handle == nullptr);
    REQUIRE_FALSE(duplicate.timedOut);
    REQUIRE(duplicate.waitedMs < 50u);

    moekoe::ReleaseInstanceLock(holder.handle);
}

TEST_CASE("Host-managed launch waits for the old instance and takes over",
          "[single-instance]") {
    const auto name = UniqueMutexName();
    const auto opt = Options(name, true, 3000);

    const auto holder = moekoe::AcquireInstanceLock(opt);
    REQUIRE(holder.result == moekoe::InstanceAcquireResult::Acquired);

    // 模拟旧 EXE 优雅退出：250ms 后关闭它持有的最后一个句柄，命名对象随
    // 之销毁。若等待循环没有在每次探测后 CloseHandle，自己持有的引用会让
    // 名字永不消失，本用例就会走到 timedOut 分支而失败。
    std::thread oldInstance([handle = holder.handle]() {
        ::Sleep(250);
        moekoe::ReleaseInstanceLock(handle);
    });

    const auto successor = moekoe::AcquireInstanceLock(opt);
    oldInstance.join();

    REQUIRE(successor.result == moekoe::InstanceAcquireResult::Acquired);
    REQUIRE(successor.handle != nullptr);
    REQUIRE_FALSE(successor.timedOut);
    REQUIRE(successor.waitedMs >= 150u);
    REQUIRE(successor.waitedMs < 3000u);

    moekoe::ReleaseInstanceLock(successor.handle);
}

TEST_CASE("Host-managed launch keeps running after takeover timeout",
          "[single-instance]") {
    const auto name = UniqueMutexName();
    const auto opt = Options(name, true, 200);

    const auto holder = moekoe::AcquireInstanceLock(opt);
    REQUIRE(holder.result == moekoe::InstanceAcquireResult::Acquired);

    // 旧实例一直不退：不能返回 DuplicateStandalone / Error，否则宿主会
    // 把 exit(0) 当作本地程序正常结束且不再拉起。
    const auto successor = moekoe::AcquireInstanceLock(opt);
    REQUIRE(successor.result == moekoe::InstanceAcquireResult::Acquired);
    REQUIRE(successor.handle != nullptr);
    REQUIRE(successor.timedOut);
    REQUIRE(successor.waitedMs >= 150u);

    moekoe::ReleaseInstanceLock(successor.handle);
    moekoe::ReleaseInstanceLock(holder.handle);
}

TEST_CASE("IsHostManagedLaunch is safe to call from any stdio setup",
          "[single-instance]") {
    bool managed = false;
    REQUIRE_NOTHROW(managed = moekoe::IsHostManagedLaunch());
    (void)managed;
}

namespace {

// 即使 REQUIRE 中途抛异常，也保证 helper 子进程被回收，不残留。
struct ChildProcessGuard {
    PROCESS_INFORMATION pi{};
    ~ChildProcessGuard() {
        if (pi.hProcess) {
            if (::WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) {
                ::TerminateProcess(pi.hProcess, 3);
                ::WaitForSingleObject(pi.hProcess, 2000);
            }
            ::CloseHandle(pi.hProcess);
        }
        if (pi.hThread) ::CloseHandle(pi.hThread);
    }
};

} // namespace

TEST_CASE("Host-managed successor takes over after a real old process exits",
          "[single-instance][process]") {
    const std::string mutexAscii = UniqueMutexNameNarrow();
    const std::string eventAscii = mutexAscii + "_ready";
    const std::wstring mutexName(mutexAscii.begin(), mutexAscii.end());
    const std::wstring eventName(eventAscii.begin(), eventAscii.end());

    HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    REQUIRE(ready != nullptr);

    wchar_t selfPath[MAX_PATH]{};
    const DWORD selfLen = ::GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    REQUIRE(selfLen > 0);
    REQUIRE(selfLen < MAX_PATH);

    std::wstring cmd = std::wstring(L"\"") + selfPath + L"\" --hold " +
                       std::wstring(mutexAscii.begin(), mutexAscii.end()) +
                       L" 700 " +
                       std::wstring(eventAscii.begin(), eventAscii.end());
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    ChildProcessGuard child;
    const BOOL created = ::CreateProcessW(
        nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &child.pi);
    REQUIRE(created != FALSE);

    // 必须等 helper 真正拿到互斥体再尝试接管，否则父进程可能抢先拿到，
    // 用例会假通过。
    const DWORD readyWait = ::WaitForSingleObject(ready, 5000);
    ::CloseHandle(ready);
    REQUIRE(readyWait == WAIT_OBJECT_0);

    const auto opt = Options(mutexName, true, 5000);
    const auto successor = moekoe::AcquireInstanceLock(opt);

    REQUIRE(successor.result == moekoe::InstanceAcquireResult::Acquired);
    REQUIRE(successor.handle != nullptr);
    REQUIRE_FALSE(successor.timedOut);
    REQUIRE(successor.waitedMs >= 100u);
    REQUIRE(successor.waitedMs < 5000u);

    moekoe::ReleaseInstanceLock(successor.handle);
}
