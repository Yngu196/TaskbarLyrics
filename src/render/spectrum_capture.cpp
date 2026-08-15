// SPDX-License-Identifier: GPL-3.0
// spectrum_capture.cpp - WASAPI loopback FFT 频谱捕获实现
//
// 采集策略（按优先级）：
//   1. 进程级 loopback（Windows 10 2004+ ActivateAudioInterfaceAsync +
//      AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK）：仅捕获 MoeKoeMusic
//      进程树（Electron 主进程 + 音频渲染子进程）的输出，排除系统其他声音。
//      目标进程退出后自动重新查找并重建采集。
//   2. 系统全局 loopback（旧方案兜底）：捕获默认输出设备的全部混音。

// 必须在 COM 头文件之前包含 windows.h
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>
#include <propidl.h>
#include <iphlpapi.h>

#include "render/spectrum_capture.h"
#include "core/constants.h"
#include "util/logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

#include <kiss_fft.h>
#include <kiss_fftr.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace moekoe {

namespace {

// FFT 点数
constexpr int FFT_SIZE = 1024;

// MoeKoeMusic 主进程名
constexpr const wchar_t* kPlayerExeName = L"MoeKoeMusic.exe";

// 目标进程退出后重新查找的间隔（毫秒）
constexpr DWORD kFindProcessRetryMs = 1000;

// 音频会话启动失败后的重试间隔（毫秒）
constexpr DWORD kSessionRetryMs = 2000;

// 进程 loopback 激活超时（毫秒）
constexpr DWORD kActivateTimeoutMs = 5000;

// 会话从未收到音频时重建会话的超时（毫秒）
// 进程树在激活时快照：Electron 的音频渲染子进程在开始播放时才
// 懒创建，若会话激活早于播放开始则收不到数据，需重建会话重新捕获
constexpr DWORD kSilentSessionTimeoutMs = 8000;

// 视为"近期无音频"的阈值（毫秒），播放提示触发重建的条件
constexpr DWORD kAudioStaleMs = 3000;

// 播放中连续无真实音频的进程会话数上限，超过后暂时切换系统兜底
constexpr int kMaxNoAudioSessions = 3;

// 系统兜底持续时间（毫秒），到期后重试进程级捕获
constexpr ULONGLONG kSystemFallbackMs = 60000;

// 系统兜底会话中播放停止多久后提前结束（避免混入其他应用声音）
constexpr ULONGLONG kSystemIdleExitMs = 5000;

// Hann 窗
void ApplyHannWindow(std::vector<float>& buf) {
    const size_t n = buf.size();
    for (size_t i = 0; i < n; ++i) {
        float w = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979323846f * static_cast<float>(i) / static_cast<float>(n - 1)));
        buf[i] *= w;
    }
}

// 对数频段合并：将 FFT 幅值 bins 映射到 numBands 个对数间距频段
// 频段内取峰值（取均值会稀释高频宽频段的幅度）
void LogBands(const std::vector<float>& magnitudes, int numBands,
              float sampleRate, std::vector<float>& outBands) {
    outBands.assign(static_cast<size_t>(numBands), 0.0f);
    const size_t numBins = magnitudes.size();
    if (numBins == 0) return;

    const float freqPerBin = sampleRate / static_cast<float>(FFT_SIZE);
    const float logMin = std::log10(constants::SPECTRUM_MIN_FREQ);
    const float logMax = std::log10(constants::SPECTRUM_MAX_FREQ);

    for (int b = 0; b < numBands; ++b) {
        const float t = static_cast<float>(b) / static_cast<float>(numBands);
        const float freqLow = std::pow(10.0f, logMin + t * (logMax - logMin));
        const float freqHigh = std::pow(10.0f, logMin + (t + 1.0f / static_cast<float>(numBands)) * (logMax - logMin));
        const int binLow = (std::max)(0, static_cast<int>(freqLow / freqPerBin));
        const int binHigh = (std::min)(static_cast<int>(numBins) - 1, static_cast<int>(freqHigh / freqPerBin));

        float peak = 0.0f;
        for (int i = binLow; i <= binHigh; ++i) {
            peak = (std::max)(peak, magnitudes[static_cast<size_t>(i)]);
        }
        outBands[static_cast<size_t>(b)] = peak;
    }
}

// 幅值 → dBFS → [0,1]
// Hann 窗下满幅正弦的 bin 峰值约为 FFT_SIZE/4，以此为 0 dBFS 基准；
// 固定 dB 区间映射取代逐帧最大值归一化，安静段不再被拉满
float MagToNormalized(float mag, float dbFloor, float dbCeil) {
    const float db = 20.0f * std::log10(mag / (FFT_SIZE / 4.0f) + 1e-9f);
    const float v = (db - dbFloor) / (dbCeil - dbFloor);
    return (std::min)(1.0f, (std::max)(0.0f, v));
}

// ─────────────────────────────────────────
// 进程查找
// ─────────────────────────────────────────

struct ProcEntry {
    DWORD       pid;
    DWORD       ppid;
    std::wstring name;
};

std::vector<ProcEntry> SnapshotProcesses() {
    std::vector<ProcEntry> procs;
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return procs;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snapshot, &pe)) {
        do {
            procs.push_back({pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile});
        } while (::Process32NextW(snapshot, &pe));
    }
    ::CloseHandle(snapshot);
    return procs;
}

bool IsPlayerExe(const std::wstring& name) {
    return _wcsicmp(name.c_str(), kPlayerExeName) == 0;
}

// 系统外壳 / 启动器 / 开发工具 / 桌面工具进程——作为父链候选时跳过
//（VS 调试启动、资源管理器手动启动等场景，避免误捕宿主进程树）
bool IsShellOrUtilityProcess(const std::wstring& name) {
    static const wchar_t* kShells[] = {
        L"explorer.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe",
        L"conhost.exe", L"openconsole.exe", L"windowsterminal.exe",
        L"svchost.exe", L"services.exe", L"wininit.exe", L"csrss.exe",
        L"smss.exe", L"winlogon.exe", L"sihost.exe", L"ctfmon.exe",
        L"runtimebroker.exe", L"applicationframehost.exe", L"dashost.exe",
        L"backgroundtaskhost.exe", L"taskhostw.exe", L"dwm.exe",
        L"python.exe", L"pythonw.exe", L"node.exe", L"npm.exe",
        // 开发工具（调试启动场景的宿主）
        L"devenv.exe", L"code.exe", L"cursor.exe", L"msbuild.exe",
        // 桌面整理 / 壁纸工具（常作为 explorer 与应用之间的中间父进程）
        L"desktopmgr64.exe", L"desktopmgr.exe", L"wallpaperengine.exe",
        L"wallpaper32.exe", L"wallpaper64.exe",
    };
    for (const wchar_t* s : kShells) {
        if (_wcsicmp(name.c_str(), s) == 0) return true;
    }
    return false;
}

// 获取进程可执行文件完整路径（失败返回空串）
std::wstring GetProcessImagePath(DWORD pid) {
    if (pid == 0) return L"";
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    const BOOL ok = ::QueryFullProcessImageNameW(h, 0, path, &size);
    ::CloseHandle(h);
    return ok ? std::wstring(path, size) : std::wstring();
}

// 路径是否包含 "moekoe"（不区分大小写）
bool PathContainsMoekoe(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring lower;
    lower.reserve(path.size());
    for (wchar_t c : path) {
        lower += static_cast<wchar_t>(std::towlower(c));
    }
    return lower.find(L"moekoe") != std::wstring::npos;
}

// 查找监听指定 TCP 端口的进程 PID（IPv4），失败返回 0。
// 播放器自带 WebSocket API 服务，通过端口反查不依赖进程名，
// 自定义构建/改名的播放器也能定位
DWORD FindListenerPidOnPort(WORD port) {
    ULONG size = 0;
    DWORD rc = ::GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                                     TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (rc != ERROR_INSUFFICIENT_BUFFER) return 0;
    std::vector<BYTE> buf(size);
    rc = ::GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                               TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (rc != NO_ERROR) return 0;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
    const WORD netPort = htons(port);
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        if (table->table[i].dwLocalPort == netPort) {
            return table->table[i].dwOwningPid;
        }
    }
    return 0;
}

// 取路径的目录部分（含结尾反斜杠），无分隔符返回空串
std::wstring DirOfPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : path.substr(0, pos + 1);
}

// 从任意进程沿父链上溯到"应用根进程"：
// 父子进程可执行文件位于同一目录时继续上溯（Electron 应用的全部
// 子进程都在同一安装目录），根进程的父进程在目录之外（explorer 等）
DWORD FindSameDirRootPid(DWORD pid, const std::vector<ProcEntry>& procs) {
    DWORD cur = pid;
    const std::wstring curDir = DirOfPath(GetProcessImagePath(cur));
    if (curDir.empty()) return pid;
    for (int depth = 0; depth < 16; ++depth) {
        DWORD ppid = 0;
        for (const auto& p : procs) {
            if (p.pid == cur) { ppid = p.ppid; break; }
        }
        if (ppid == 0) break;
        const std::wstring parentDir = DirOfPath(GetProcessImagePath(ppid));
        if (parentDir.empty() ||
            _wcsicmp(parentDir.c_str(), curDir.c_str()) != 0) {
            break;
        }
        cur = ppid;
    }
    return cur;
}

// 查找 MoeKoeMusic 根进程（Electron 主进程）PID，未找到返回 0
// 按优先级：
//   1) 父链上的 MoeKoeMusic.exe（本 exe 由播放器作为 native host 直接拉起）；
//   2) 父链上安装路径包含 "moekoe" 的祖先（播放器 exe 被改名时）；
//   3) WebSocket API 端口的监听进程 → 上溯到同目录根进程
//      ——不依赖进程名，本 exe 被 VS 调试/手动启动、播放器独立运行时
//        也能准确定位（端口即我们正在连接的播放器服务）；
//   4) 快照中找 MoeKoeMusic.exe 根进程（父进程不是 MoeKoeMusic.exe 的）；
//   5) 快照中任意 MoeKoeMusic.exe；
//   6) 兜底：父链上最近的非外壳/非开发工具祖先（最后手段）
// logChain 为 true 时记录父链（诊断用）；outName 返回目标进程名
DWORD FindPlayerRootPid(int wsPort, bool logChain = false,
                        std::wstring* outName = nullptr) {
    if (outName) outName->clear();
    const std::vector<ProcEntry> procs = SnapshotProcesses();
    if (procs.empty()) return 0;

    // 先完整走一遍父链（记录诊断日志）
    std::vector<const ProcEntry*> chain;
    DWORD cur = ::GetCurrentProcessId();
    for (int depth = 0; cur != 0 && depth < 16; ++depth) {
        const ProcEntry* self = nullptr;
        for (const auto& p : procs) {
            if (p.pid == cur) { self = &p; break; }
        }
        if (!self) break;
        if (logChain) {
            Log("[Spectrum]   ancestor[%d] pid=%lu name='%ls'\n",
                depth, cur, self->name.c_str());
        }
        chain.push_back(self);
        cur = self->ppid;
    }

    // 1) 父链上的 MoeKoeMusic.exe（跳过自身，从父进程开始）
    for (size_t i = 1; i < chain.size(); ++i) {
        if (IsPlayerExe(chain[i]->name)) {
            if (outName) *outName = chain[i]->name;
            return chain[i]->pid;
        }
    }

    // 2) 父链上安装路径包含 "moekoe" 的最近祖先
    for (size_t i = 1; i < chain.size(); ++i) {
        if (PathContainsMoekoe(GetProcessImagePath(chain[i]->pid))) {
            if (outName) *outName = chain[i]->name + L" (by-path)";
            return chain[i]->pid;
        }
    }

    // 3) WebSocket API 端口监听进程 → 同目录根进程（不依赖进程名）
    if (wsPort > 0 && wsPort <= 65535) {
        const DWORD listenerPid = FindListenerPidOnPort(static_cast<WORD>(wsPort));
        if (listenerPid != 0) {
            const DWORD rootPid = FindSameDirRootPid(listenerPid, procs);
            for (const auto& p : procs) {
                if (p.pid == rootPid) {
                    if (outName) *outName = p.name + L" (ws-listener)";
                    return rootPid;
                }
            }
        }
    }

    // 4) 快照根进程识别（父进程非 MoeKoeMusic.exe 的即为主进程）
    for (const auto& p : procs) {
        if (!IsPlayerExe(p.name)) continue;
        bool parentIsPlayer = false;
        for (const auto& q : procs) {
            if (q.pid == p.ppid && IsPlayerExe(q.name)) { parentIsPlayer = true; break; }
        }
        if (!parentIsPlayer) {
            if (outName) *outName = p.name;
            return p.pid;
        }
    }

    // 5) 快照兜底：任意 MoeKoeMusic.exe
    for (const auto& p : procs) {
        if (IsPlayerExe(p.name)) {
            if (outName) *outName = p.name;
            return p.pid;
        }
    }

    // 6) 父链上最近的非外壳/非开发工具祖先（最后手段）
    for (size_t i = 1; i < chain.size(); ++i) {
        if (!IsShellOrUtilityProcess(chain[i]->name)) {
            if (outName) *outName = chain[i]->name + L" (guessed)";
            return chain[i]->pid;
        }
    }
    return 0;
}

bool IsProcessAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const BOOL ok = ::GetExitCodeProcess(h, &code);
    ::CloseHandle(h);
    return ok && code == STILL_ACTIVE;
}

// ─────────────────────────────────────────
// 采样格式
// ─────────────────────────────────────────

struct SampleFormat {
    bool   isFloat = true;   // float32（false 为 int16）
    UINT32 channels = 2;
    DWORD  sampleRate = 48000;
};

// 仅支持 float32 / int16 混音格式（覆盖几乎所有系统默认配置）
bool ParseWaveFormat(const WAVEFORMATEX* wfx, SampleFormat* out) {
    if (!wfx || wfx->nChannels == 0 || wfx->nSamplesPerSec == 0) return false;

    bool isFloat = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE* ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    if (!isFloat && wfx->wBitsPerSample != 16) return false;

    out->isFloat = isFloat;
    out->channels = wfx->nChannels;
    out->sampleRate = wfx->nSamplesPerSec;
    return true;
}

// ─────────────────────────────────────────
// 进程级 loopback 异步激活
// ─────────────────────────────────────────

// ActivateAudioInterfaceAsync 完成回调（MTA 线程池线程调用）
// 额外实现 IAgileObject 标记接口以免除跨套间封送要求
class ActivateHandler final
    : public IActivateAudioInterfaceCompletionHandler,
      public IAgileObject {
public:
    explicit ActivateHandler(HANDLE doneEvent)
        : doneEvent_(doneEvent), refCount_(1) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ::InterlockedIncrement(&refCount_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG c = ::InterlockedDecrement(&refCount_);
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IUnknown*>(
                static_cast<IActivateAudioInterfaceCompletionHandler*>(this));
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IActivateAudioInterfaceCompletionHandler
    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hrActivate = E_UNEXPECTED;
        IUnknown* punk = nullptr;
        HRESULT hr = op->GetActivateResult(&hrActivate, &punk);
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && punk) {
            hr = punk->QueryInterface(__uuidof(IAudioClient),
                                      reinterpret_cast<void**>(&client_));
            punk->Release();
        } else {
            hr = FAILED(hr) ? hr : hrActivate;
        }
        result_ = hr;
        ::SetEvent(doneEvent_);
        return S_OK;
    }

    IAudioClient* DetachClient() {
        IAudioClient* c = client_;
        client_ = nullptr;
        return c;
    }
    HRESULT Result() const { return result_; }

private:
    ~ActivateHandler() = default;  // 仅能通过 Release 释放

    HANDLE      doneEvent_;
    IAudioClient* client_ = nullptr;
    HRESULT     result_ = E_FAIL;
    LONG        refCount_;
};

// ─────────────────────────────────────────
// 音频会话建立
// ─────────────────────────────────────────

struct CaptureSession {
    IAudioClient*        client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    SampleFormat         fmt;
};

// 进程级 loopback：仅捕获 pid 进程树的音频输出
HRESULT StartProcessLoopback(DWORD pid, CaptureSession* out) {
    HANDLE doneEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!doneEvent) return E_FAIL;

    ActivateHandler* handler = new (std::nothrow) ActivateHandler(doneEvent);
    if (!handler) {
        ::CloseHandle(doneEvent);
        return E_OUTOFMEMORY;
    }

    // 指定采集格式：立体声 float32（可直接喂 FFT）
    // AUTOCONVERTPCM 使音频引擎自动转换到请求的格式
    SampleFormat fmt;
    fmt.isFloat = true;
    fmt.channels = 2;
    fmt.sampleRate = 48000;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels = static_cast<WORD>(fmt.channels);
    wfx.nSamplesPerSec = fmt.sampleRate;
    wfx.wBitsPerSample = 32;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    // 包含目标进程树：Electron 的音频由主进程的子进程渲染
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams{};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    HRESULT hr = ::ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &activateParams, handler, &asyncOp);

    if (SUCCEEDED(hr)) {
        if (::WaitForSingleObject(doneEvent, kActivateTimeoutMs) != WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            hr = handler->Result();
        }
    }

    if (SUCCEEDED(hr)) {
        IAudioClient* client = handler->DetachClient();
        hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            0, 0, &wfx, nullptr);
        if (SUCCEEDED(hr)) {
            hr = client->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void**>(&out->capture));
        }
        if (SUCCEEDED(hr)) {
            hr = client->Start();
        }
        if (SUCCEEDED(hr)) {
            out->client = client;
            out->fmt = fmt;
        } else {
            client->Release();
        }
    }

    if (asyncOp) asyncOp->Release();
    handler->Release();
    ::CloseHandle(doneEvent);
    return hr;
}

// 系统全局 loopback（兜底）：捕获默认输出设备的全部混音
HRESULT StartSystemLoopback(CaptureSession* out) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return FAILED(hr) ? hr : E_FAIL;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr) || !device) return FAILED(hr) ? hr : E_FAIL;

    IAudioClient* client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr) || !client) {
        if (client) client->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = client->GetMixFormat(&pwfx);
    if (SUCCEEDED(hr) && !ParseWaveFormat(pwfx, &out->fmt)) {
        hr = E_NOINTERFACE;  // 不支持的混音格式
    }
    if (SUCCEEDED(hr)) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK,
                                100000, 0, pwfx, nullptr);  // 10ms 缓冲
    }
    if (SUCCEEDED(hr)) {
        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(&out->capture));
    }
    if (SUCCEEDED(hr)) {
        hr = client->Start();
    }
    if (pwfx) ::CoTaskMemFree(pwfx);
    if (SUCCEEDED(hr)) {
        out->client = client;
    } else {
        client->Release();
    }
    return hr;
}

// 分片休眠，随时响应停止请求
void InterruptibleSleep(const SpectrumCapture* parent, DWORD totalMs) {
    for (DWORD slept = 0; slept < totalMs; slept += 50) {
        if (!parent->IsRunning()) return;
        ::Sleep((std::min<DWORD>)(50, totalMs - slept));
    }
}

} // namespace

struct SpectrumCapture::Impl {
    std::unique_ptr<std::thread> captureThread;
    std::vector<float>          spectrumOutput;
    std::vector<float>          smoothSpectrum;
    mutable std::mutex          spectrumMutex;

    static constexpr size_t kRingBufferSize = FFT_SIZE * 2;
    std::vector<float>          ringBuffer;
    size_t                      ringWritePos{0};
    size_t                      ringSamplesAvailable{0};
    std::mutex                  ringMutex;

    DWORD                       targetPid{0};
    bool                        isProcessLoopback{false};
    SampleFormat                fmt;

    // 播放提示（主线程 → 采集线程）：请求重建音频会话
    std::atomic<bool>           reactivateHint{false};

    // MoeKoeMusic WebSocket API 端口（用于反查播放器进程）
    int                         wsPort{6520};

    // 进程级捕获连续无音频的会话计数与系统兜底截止时间
    int                         noAudioSessions{0};
    int                         consecutiveFallbacks{0};
    ULONGLONG                   systemFallbackUntil{0};

    // FFT 复用缓冲区（避免逐帧堆分配）
    std::vector<float>          fftInput;
    std::vector<kiss_fft_cpx>   fftOut;
    std::vector<float>          fftMags;
    std::vector<float>          bandScratch;

    // 运行时可调参数（由 SetParams 更新）
    float                       dbCeil{constants::SPECTRUM_DB_CEIL};
    float                       dbFloor{constants::SPECTRUM_DB_FLOOR};
    int                         numBands{constants::SPECTRUM_NUM_BANDS};
    std::atomic<bool>           paramsDirty{false};

    void PushMonoLocked(float v) {
        ringBuffer[ringWritePos] = v;
        ringWritePos = (ringWritePos + 1) % kRingBufferSize;
        if (ringSamplesAvailable < kRingBufferSize) ++ringSamplesAvailable;
    }

    // 多声道交织数据下混为 mono 写入环形缓冲
    void PushInterleaved(const BYTE* data, UINT32 frames) {
        const UINT32 ch = fmt.channels;
        std::lock_guard<std::mutex> lock(ringMutex);
        if (fmt.isFloat) {
            const float* s = reinterpret_cast<const float*>(data);
            for (UINT32 f = 0; f < frames; ++f) {
                float sum = 0.0f;
                for (UINT32 c = 0; c < ch; ++c) sum += s[f * ch + c];
                PushMonoLocked(sum / static_cast<float>(ch));
            }
        } else {
            const int16_t* s = reinterpret_cast<const int16_t*>(data);
            for (UINT32 f = 0; f < frames; ++f) {
                float sum = 0.0f;
                for (UINT32 c = 0; c < ch; ++c) sum += static_cast<float>(s[f * ch + c]);
                PushMonoLocked(sum / static_cast<float>(ch) / 32768.0f);
            }
        }
    }

    // 静音帧写入零样本，使频谱随 EMA 自然衰减
    void PushSilence(UINT32 frames) {
        std::lock_guard<std::mutex> lock(ringMutex);
        for (UINT32 f = 0; f < frames; ++f) {
            PushMonoLocked(0.0f);
        }
    }

    // 消费 FFT_SIZE 个样本执行频谱分析并发布结果
    void RunFftPass(kiss_fftr_cfg fftCfg) {
        if (!fftCfg) return;

        size_t available;
        {
            std::lock_guard<std::mutex> lock(ringMutex);
            available = ringSamplesAvailable;
        }
        if (available < static_cast<size_t>(FFT_SIZE)) return;

        {
            std::lock_guard<std::mutex> lock(ringMutex);
            size_t readPos = (ringWritePos + kRingBufferSize - available) % kRingBufferSize;
            for (int i = 0; i < FFT_SIZE; ++i) {
                fftInput[static_cast<size_t>(i)] = ringBuffer[readPos];
                readPos = (readPos + 1) % kRingBufferSize;
            }
            ringSamplesAvailable -= FFT_SIZE;
        }

        ApplyHannWindow(fftInput);

        // kiss_fftr: 实数 FFT（输入 N 个实数，输出 N/2+1 个复数）
        kiss_fftr(fftCfg, fftInput.data(), fftOut.data());

        for (int i = 0; i < FFT_SIZE / 2; ++i) {
            const float re = fftOut[static_cast<size_t>(i)].r;
            const float im = fftOut[static_cast<size_t>(i)].i;
            fftMags[static_cast<size_t>(i)] = std::sqrt(re * re + im * im);
        }

        // 参数变更时重新分配缓冲区
        if (paramsDirty.exchange(false)) {
            bandScratch.assign(static_cast<size_t>(numBands), 0.0f);
            smoothSpectrum.clear();  // 重新初始化平滑缓冲
        }

        LogBands(fftMags, numBands,
                 static_cast<float>(fmt.sampleRate), bandScratch);
        for (float& v : bandScratch) v = MagToNormalized(v, dbFloor, dbCeil);

        // 非对称平滑：上升快（跟拍），下降慢（余晖）
        if (smoothSpectrum.size() != bandScratch.size()) {
            smoothSpectrum = bandScratch;
        } else {
            for (size_t i = 0; i < bandScratch.size(); ++i) {
                const float keep = (bandScratch[i] > smoothSpectrum[i])
                    ? constants::SPECTRUM_SMOOTH_ATTACK
                    : constants::SPECTRUM_SMOOTH_RELEASE;
                smoothSpectrum[i] = smoothSpectrum[i] * keep +
                                    bandScratch[i] * (1.0f - keep);
            }
        }

        std::lock_guard<std::mutex> lock(spectrumMutex);
        spectrumOutput = smoothSpectrum;
    }

    void ResetSpectrum() {
        std::lock_guard<std::mutex> lock(spectrumMutex);
        spectrumOutput.clear();
        smoothSpectrum.clear();
    }

    void CaptureLoop(SpectrumCapture* parent);
};

void SpectrumCapture::Impl::CaptureLoop(SpectrumCapture* parent) {
    // 启动标识日志：用于确认新版本二进制实际生效
    Log("[Spectrum] Capture thread started (proc-loopback v2)\n");

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCom)) {
        Log("[Spectrum] CoInitializeEx failed: 0x%08X\n", hrCom);
        parent->running_ = false;
        return;
    }

    // FFT 配置整个生命周期只分配一次
    kiss_fftr_cfg fftCfg = kiss_fftr_alloc(FFT_SIZE, 0, nullptr, nullptr);
    if (!fftCfg) {
        Log("[Spectrum] kiss_fftr_alloc failed\n");
        CoUninitialize();
        parent->running_ = false;
        return;
    }

    fftInput.assign(FFT_SIZE, 0.0f);
    fftOut.assign(FFT_SIZE / 2 + 1, kiss_fft_cpx{0.0f, 0.0f});
    fftMags.assign(FFT_SIZE / 2, 0.0f);
    bandScratch.assign(static_cast<size_t>(numBands), 0.0f);

    bool firstFindAttempt = true;   // 首次查找时输出父链诊断日志
    int  findRetryCount = 0;        // 未找到目标进程的重试计数（用于日志限频）

    while (parent->running_.load()) {
        CaptureSession session;

        // 1) 会话来源选择：
        //    - 限时系统兜底期内：直接用系统全局 loopback（保证频谱可见）
        //    - 否则：首选进程级 loopback（仅捕获 MoeKoeMusic 进程树），
        //      激活失败且目标进程存在时回退系统全局
        const bool timedFallback = ::GetTickCount64() < systemFallbackUntil;
        bool systemTimed = false;
        HRESULT hr = E_FAIL;
        isProcessLoopback = false;
        targetPid = 0;

        if (timedFallback) {
            hr = StartSystemLoopback(&session);
            systemTimed = SUCCEEDED(hr);
            if (systemTimed) {
                Log("[Spectrum] System loopback started (timed fallback)\n");
            }
        } else {
            if (firstFindAttempt) {
                Log("[Spectrum] Locating player process; parent chain:\n");
            }
            std::wstring targetName;
            targetPid = FindPlayerRootPid(wsPort, firstFindAttempt, &targetName);
            firstFindAttempt = false;
            if (targetPid == 0) {
                // 找不到 MoeKoeMusic 进程：等待其出现并重试（日志限频 ~30s 一次）
                if ((findRetryCount++ % 15) == 0) {
                    Log("[Spectrum] Player process not found (retry %d)\n", findRetryCount);
                }
            } else {
                findRetryCount = 0;
            }
            if (targetPid != 0) {
                hr = StartProcessLoopback(targetPid, &session);
                if (SUCCEEDED(hr)) {
                    isProcessLoopback = true;
                    Log("[Spectrum] Process loopback started (pid=%lu name='%ls')\n",
                        targetPid, targetName.c_str());
                } else {
                    Log("[Spectrum] Process loopback failed (pid=%lu): 0x%08X, fallback to system\n",
                        targetPid, hr);
                }
            }

            // 激活失败回退：仅当目标进程存在但进程级激活失败时才回退；
            // MoeKoeMusic 未运行时不采集（等待进程出现并重试），
            // 避免又混入系统其他声音
            if (FAILED(hr) && targetPid != 0 && parent->running_.load()) {
                hr = StartSystemLoopback(&session);
                if (SUCCEEDED(hr)) {
                    Log("[Spectrum] System loopback started (activation fallback)\n");
                }
            }
        }

        if (FAILED(hr) || !parent->running_.load()) {
            if (session.capture) session.capture->Release();
            if (session.client) session.client->Release();
            ResetSpectrum();
            InterruptibleSleep(parent, kSessionRetryMs);
            continue;
        }

        fmt = session.fmt;
        {
            std::lock_guard<std::mutex> lock(ringMutex);
            ringWritePos = 0;
            ringSamplesAvailable = 0;
            std::fill(ringBuffer.begin(), ringBuffer.end(), 0.0f);
        }
        smoothSpectrum.clear();

        // 3) 采集主循环（目标进程退出、播放提示重建或捕获错误时结束，返回外层重建）
        DWORD tick = 0;
        bool sessionAlive = true;
        bool everAudio = false;                 // 本会话是否收到过有效音频
        bool hintSeen = false;                  // 本会话期间是否收到播放提示
        ULONGLONG sessionStart = ::GetTickCount64();
        ULONGLONG lastAudio = sessionStart;
        ULONGLONG lastHint = 0;
        DWORD rebuildDelayMs = kFindProcessRetryMs;
        while (sessionAlive && parent->running_.load()) {
            ::Sleep(16); // ~60fps
            ++tick;
            const ULONGLONG now = ::GetTickCount64();

            // 每 ~1s 检查一次目标进程是否仍在（仅进程模式）
            if (isProcessLoopback && (tick % 64) == 0 && !IsProcessAlive(targetPid)) {
                Log("[Spectrum] Target process exited (pid=%lu)\n", targetPid);
                break;
            }

            // 播放提示：近期无真实音频时立即重建会话，
            // 重新激活以纳入播放开始后才启动的音频渲染子进程
            if (reactivateHint.exchange(false)) {
                hintSeen = true;
                lastHint = now;
                if (isProcessLoopback && lastAudio + kAudioStaleMs < now) {
                    Log("[Spectrum] Playback hint: rebuilding session\n");
                    rebuildDelayMs = 200;
                    break;
                }
            }

            // 限时系统兜底会话：播放已真正停止（既无播放提示、也无音频数据）
            // 一段时间后提前结束，避免继续混入其他应用的声音。
            // 注意：频谱正常时主线程不会再发提示，故必须同时检查无音频，
            // 否则音乐播放中会被误杀
            if (systemTimed &&
                lastHint + kSystemIdleExitMs < now &&
                lastAudio + kSystemIdleExitMs < now &&
                sessionStart + kSystemIdleExitMs < now) {
                Log("[Spectrum] Playback idle, ending system fallback session\n");
                systemFallbackUntil = 0;
                break;
            }

            // 看门狗：会话建立后始终未收到音频，超时重建（兜底）
            if (!everAudio && sessionStart + kSilentSessionTimeoutMs < now) {
                Log("[Spectrum] No audio received, rebuilding session\n");
                rebuildDelayMs = 200;
                break;
            }

            UINT32 packetLength = 0;
            bool gotAudioThisTick = false;
            while (sessionAlive) {
                HRESULT hrPacket = session.capture->GetNextPacketSize(&packetLength);
                if (FAILED(hrPacket)) { sessionAlive = false; break; }
                if (packetLength == 0) break;

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hrPacket = session.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hrPacket)) { sessionAlive = false; break; }

                if (frames > 0) {
                    // SILENT 标记时 data 可能为 nullptr，统一按静音处理
                    if (!data || (flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        PushSilence(frames);
                    } else {
                        PushInterleaved(data, frames);
                        gotAudioThisTick = true;
                    }
                }

                hrPacket = session.capture->ReleaseBuffer(frames);
                if (FAILED(hrPacket)) { sessionAlive = false; break; }
            }

            if (gotAudioThisTick) {
                everAudio = true;
                lastAudio = ::GetTickCount64();
            }
            if (sessionAlive) RunFftPass(fftCfg);
        }

        session.client->Stop();
        session.capture->Release();
        session.client->Release();

        // 会话音频记账：播放中（收到过提示）的进程会话若始终无真实音频，
        // 连续多次后暂时切换系统兜底，保证频谱可见；
        // 连续触发时逐次延长兜底时长（60s → 2min → ... → 16min 上限），
        // 减少频谱周期性中断
        if (isProcessLoopback) {
            if (everAudio) {
                noAudioSessions = 0;
                consecutiveFallbacks = 0;
            } else if (hintSeen) {
                if (++noAudioSessions >= kMaxNoAudioSessions) {
                    const int shift = (std::min)(consecutiveFallbacks, 4);
                    systemFallbackUntil = ::GetTickCount64() + (kSystemFallbackMs << shift);
                    ++consecutiveFallbacks;
                    noAudioSessions = 0;
                    Log("[Spectrum] Process loopback silent while playing, "
                        "switching to system fallback\n");
                }
            }
        }

        // 会话结束：曾收到音频的快速重建不清空频谱，避免闪烁到 "..."
        if (!everAudio) ResetSpectrum();
        InterruptibleSleep(parent, rebuildDelayMs);
    }

    free(fftCfg);
    CoUninitialize();
    Log("[Spectrum] Capture stopped\n");
}

SpectrumCapture::SpectrumCapture()
    : impl_(std::make_unique<Impl>()) {
    impl_->ringBuffer.resize(Impl::kRingBufferSize, 0.0f);
}

SpectrumCapture::~SpectrumCapture() {
    Stop();
}

bool SpectrumCapture::Start(int wsPort) {
    if (running_.load()) return true;

    impl_->wsPort = wsPort;
    running_ = true; // 先置位避免阻塞调用方
    impl_->captureThread = std::make_unique<std::thread>(
        &Impl::CaptureLoop, impl_.get(), this);
    return true;
}

void SpectrumCapture::Stop() {
    if (!running_.load()) return;

    running_ = false;
    if (impl_->captureThread && impl_->captureThread->joinable()) {
        DWORD waitResult = ::WaitForSingleObject(
            impl_->captureThread->native_handle(),
            moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
        if (waitResult == WAIT_TIMEOUT) {
            Log("[Spectrum] Thread join timed out (%d ms), detaching\n",
                moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
            impl_->captureThread->detach();
        } else {
            impl_->captureThread->join();
        }
    }
    impl_->captureThread.reset();
}

void SpectrumCapture::NotifyPlaybackActive() {
    impl_->reactivateHint.store(true);
}

void SpectrumCapture::SetParams(float dbCeil, float dbFloor, int numBands) {
    impl_->dbCeil  = dbCeil;
    impl_->dbFloor = dbFloor;
    impl_->numBands = numBands;
    impl_->paramsDirty.store(true);
}

std::vector<float> SpectrumCapture::GetSpectrum(int numBands) {
    if (!running_.load()) return {};

    std::lock_guard<std::mutex> lock(impl_->spectrumMutex);
    if (impl_->spectrumOutput.empty()) return {};

    // 如果请求的频段数与内部不同，重新映射
    if (numBands == static_cast<int>(impl_->spectrumOutput.size())) {
        return impl_->spectrumOutput;
    }

    // 简单下采样/上采样
    std::vector<float> result(static_cast<size_t>(numBands), 0.0f);
    const auto& src = impl_->spectrumOutput;
    for (int i = 0; i < numBands; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(numBands);
        size_t srcIdx = static_cast<size_t>(t * static_cast<float>(src.size() - 1));
        if (srcIdx < src.size()) {
            result[static_cast<size_t>(i)] = src[srcIdx];
        }
    }
    return result;
}

} // namespace moekoe
