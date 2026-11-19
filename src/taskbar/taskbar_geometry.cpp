// SPDX-License-Identifier: GPL-3.0
// taskbar_geometry.cpp - 任务栏几何信息与 UIA 枚举实现
//
// 任务栏方位判断算法：
//   - 水平任务栏（BOTTOM / TOP）：tbWidth > tbHeight，再按 tb.top 与 rcWork 中心线比大小
//   - 垂直任务栏（LEFT / RIGHT）：tbWidth ≤ tbHeight，按 tb.left 与 rcWork 中心线比大小
//
// UIA 枚举线程模型：
//   所有 IUIAutomation 调用（ElementFromHandle / FindAll / BoundingRectangle 均为
//   无超时的跨进程 COM）只在后台工作线程执行。工作线程自建 COM（MTA）与
//   IUIAutomation 实例；线程函数只捕获 shared_ptr<UiaWorkerState> 副本，
//   即使在 Shutdown 时被分离（UIA 调用可能无限期阻塞），也不会触碰已析构的
//   TaskbarGeometry 对象。UI 线程兜底走 EnumChildWindows（不发送跨进程消息）。
//
// 子窗口结构（Win11 任务栏为虚拟化子窗口）：
//   MSTaskListWClass              → 任务列表（应用图标区），需要右侧 edge 计算可用宽度
//   DesktopWindowContentBridge    → Win11 24H2+ 任务列表的替代类名（非标准）
//   TrayNotifyWnd                 → 系统托盘（时钟、通知图标）
//   ReBarWindow32                 → 工具栏容器
#include "taskbar/taskbar_geometry.h"

#include <shellapi.h>
#include <windows.h>

#include "util/logger.h"

namespace moekoe {

namespace {

// ── EnumChildWindows 枚举路径（UI 线程安全：GetClassNameW/GetWindowRect
//    不向其他线程的窗口发送消息） ──
bool EnumQueryChildRects(HWND hTaskbar,
                         RECT& tl, bool& fTL, RECT& tr, bool& fTR,
                         RECT& rb, bool& fRB, int tbW) {
    fTL = fTR = fRB = false;
    HWND hChild = ::GetWindow(hTaskbar, GW_CHILD);
    while (hChild) {
        if (::IsWindowVisible(hChild)) {
            wchar_t name[256] = {};
            ::GetClassNameW(hChild, name, 256);
            RECT cr{};
            ::GetWindowRect(hChild, &cr);
            if (wcscmp(name, L"MSTaskListWClass") == 0) {
                tl = cr; fTL = true;
            } else if (wcscmp(name, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
                // Win11 虚拟化任务栏中该 wrapper 窗覆盖全宽和任务列表区域，
                // 宽度 < tbWidth - 10 的区分任务列表 wrapper 和托盘 wrapper
                if ((cr.right - cr.left) < tbW - 10) { tl = cr; fTL = true; }
            } else if (wcscmp(name, L"TrayNotifyWnd") == 0) {
                tr = cr; fTR = true;
            } else if (wcscmp(name, L"ReBarWindow32") == 0) {
                rb = cr; fRB = true;
            }
        }
        hChild = ::GetWindow(hChild, GW_HWNDNEXT);
    }
    return true;
}

// 仅查找任务列表（帧锁定二次采样的枚举兜底）
bool EnumFindTaskListOnly(HWND hTaskbar, int tbW, RECT& out) {
    HWND hChild = ::GetWindow(hTaskbar, GW_CHILD);
    while (hChild) {
        if (::IsWindowVisible(hChild)) {
            wchar_t cname[256] = {};
            ::GetClassNameW(hChild, cname, 255);
            bool match = (wcscmp(cname, L"MSTaskListWClass") == 0);
            if (!match && wcscmp(cname,
                    L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
                RECT cr{};
                ::GetWindowRect(hChild, &cr);
                if ((cr.right - cr.left) < tbW - 10) match = true;
            }
            if (match) {
                ::GetWindowRect(hChild, &out);
                return true;
            }
        }
        hChild = ::GetWindow(hChild, GW_HWNDNEXT);
    }
    return false;
}

// ── UIA 枚举路径（仅工作线程调用） ──
bool UiaQueryChildRects(IUIAutomation* uia, HWND hTaskbar,
                        RECT& tl, bool& fTL, RECT& tr, bool& fTR,
                        RECT& rb, bool& fRB, int tbW) {
    fTL = fTR = fRB = false;
    if (!uia) return false;

    IUIAutomationElement* tbElem = nullptr;
    if (FAILED(uia->ElementFromHandle(hTaskbar, &tbElem)) || !tbElem) return false;

    // 辅助 lambda：按 ClassName 查找子元素，提取其 CurrentBoundingRectangle
    // maxW 用于 DesktopWindowContentBridge 去重（区分任务列表和托盘包装器）
    auto findClass = [&](const wchar_t* cls, RECT& out, bool& found, int maxW = 0) {
        VARIANT v; v.vt = VT_BSTR; v.bstrVal = ::SysAllocString(cls);
        IUIAutomationCondition* c = nullptr;
        if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ClassNamePropertyId, v, &c)) && c) {
            ::VariantClear(&v);
            IUIAutomationElementArray* a = nullptr;
            if (SUCCEEDED(tbElem->FindAll(TreeScope_Children, c, &a)) && a) {
                int n = 0; a->get_Length(&n);
                for (int i = 0; i < n && !found; i++) {
                    IUIAutomationElement* m = nullptr; a->GetElement(i, &m);
                    if (m) {
                        RECT cr{}; m->get_CurrentBoundingRectangle(&cr);
                        if (maxW <= 0 || (cr.right - cr.left) < maxW) { out = cr; found = true; }
                        m->Release();
                    }
                }
                a->Release();
            }
            c->Release();
        } else ::VariantClear(&v);
    };

    // 按优先级查找：任务列表 > 系统托盘 > 工具栏
    findClass(L"MSTaskListWClass", tl, fTL);
    // Win11 24H2+：MSTaskListWClass 被虚拟化，改为 DesktopWindowContentBridge
    if (!fTL) findClass(L"Windows.UI.Composition.DesktopWindowContentBridge", tl, fTL, tbW - 10);
    findClass(L"TrayNotifyWnd", tr, fTR);
    findClass(L"ReBarWindow32", rb, fRB);

    tbElem->Release();
    return true;
}

// 单次完整采样：UIA 可用走 UIA，否则 EnumChildWindows 兜底
void SampleChildRects(IUIAutomation* uia, HWND hTaskbar, int tbW, UiaSnapshot& s) {
    if (!UiaQueryChildRects(uia, hTaskbar, s.taskList, s.hasTaskList,
                            s.tray, s.hasTray, s.rebar, s.hasRebar, tbW)) {
        EnumQueryChildRects(hTaskbar, s.taskList, s.hasTaskList,
                            s.tray, s.hasTray, s.rebar, s.hasRebar, tbW);
    }
}

// ── 工作线程主循环：只访问 st（shared_ptr），不触碰 TaskbarGeometry ──
void UiaWorkerLoop(std::shared_ptr<UiaWorkerState> st) {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInit = SUCCEEDED(hr);
    IUIAutomation* uia = nullptr;
    if (comInit) {
        ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
        if (!uia) {
            Log("[UIA-WORKER] CoCreateInstance(CUIAutomation) failed, fallback to EnumChildWindows\n");
        }
    }

    for (;;) {
        HWND hwnd = nullptr;
        int  tbW = 0;
        bool stability = false;
        {
            std::unique_lock<std::mutex> lk(st->mtx);
            st->cv.wait(lk, [&st] { return st->stop || st->pending; });
            if (st->stop) break;
            st->pending = false;
            hwnd = st->hwnd; tbW = st->tbWidth; stability = st->stability;
        }

        UiaSnapshot s;
        s.valid = (hwnd != nullptr);
        if (s.valid) {
            SampleChildRects(uia, hwnd, tbW, s);

            // 稳定性二次采样：2ms 后重复测量任务栏本体与任务列表
            if (stability) {
                ::Sleep(2);
                RECT tbC{};
                s.tbCheckValid = ::GetWindowRect(hwnd, &tbC) != 0;
                s.tbCheck = tbC;
                RECT tl2{}, tr2{}, rb2{};
                bool fTL2 = false, fTR2 = false, fRB2 = false;
                if (UiaQueryChildRects(uia, hwnd, tl2, fTL2, tr2, fTR2, rb2, fRB2, tbW) && fTL2) {
                    s.taskListCheck = tl2;
                    s.taskListCheckValid = true;
                } else {
                    // UIA 二查失败：EnumChildWindows 兜底
                    s.taskListCheckValid = EnumFindTaskListOnly(hwnd, tbW, s.taskListCheck);
                }
            }
        }
        s.tickMs = ::GetTickCount64();

        {
            std::lock_guard<std::mutex> lk(st->mtx);
            st->snap = s;
        }
    }

    if (uia) uia->Release();
    if (comInit) ::CoUninitialize();
}

} // namespace

TaskbarGeometry::TaskbarGeometry() = default;
TaskbarGeometry::~TaskbarGeometry() { ShutdownUiaWorker(); }

HWND TaskbarGeometry::FindTaskbarHandle() {
    // 主任务栏
    HWND h = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (h) return h;
    // 多显示器：副屏任务栏
    return ::FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
}

TaskbarInfo TaskbarGeometry::Detect(HWND hTaskbar) {
    if (!hTaskbar) return info_;

    // 获取任务栏所在显示器的 rcWork，用于判断方位（中心线基准）
    HMONITOR mon = ::MonitorFromWindow(hTaskbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(mon, &mi);

    RECT tb{}; ::GetWindowRect(hTaskbar, &tb);
    info_.rect = tb;

    const int tbW = tb.right - tb.left, tbH = tb.bottom - tb.top;

    // 宽 > 高 → 水平任务栏；以 rcWork 垂直中心线判断 TOP vs BOTTOM
    if (tbW > tbH)
        info_.position = (tb.top >= (mi.rcWork.bottom + mi.rcWork.top) / 2)
            ? TaskbarPosition::BOTTOM : TaskbarPosition::TOP;
    // 宽 ≤ 高 → 垂直任务栏；以 rcWork 水平中心线判断 LEFT vs RIGHT
    else
        info_.position = (tb.left >= (mi.rcWork.right + mi.rcWork.left) / 2)
            ? TaskbarPosition::RIGHT : TaskbarPosition::LEFT;

    info_.dpi = static_cast<UINT>(::GetDpiForWindow(hTaskbar));

    // ABS_AUTOHIDE 查询任务栏自动隐藏状态
    APPBARDATA abd{}; abd.cbSize = sizeof(abd);
    info_.autoHide = (::SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;

    return info_;
}

void TaskbarGeometry::RequestUiaRefresh(HWND hTaskbar, int tbWidth, bool stability,
                                        int intervalMs) {
    if (!hTaskbar) return;

    // 节流：intervalMs 内的重复请求直接忽略（仅 UI 线程调用，无竞争）
    const ULONGLONG now = ::GetTickCount64();
    if (state_ && now - lastRequestTick_ < static_cast<ULONGLONG>(intervalMs)) return;
    lastRequestTick_ = now;

    if (worker_.joinable()) {
        std::lock_guard<std::mutex> lk(state_->mtx);
        if (state_->stop) return;
        state_->pending = true;
        state_->hwnd = hTaskbar;
        state_->tbWidth = tbWidth;
        state_->stability = stability;
        state_->cv.notify_one();
        return;
    }

    // 首次请求：启动工作线程（自带待处理请求，避免空转一轮）
    state_ = std::make_shared<UiaWorkerState>();
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        state_->pending = true;
        state_->hwnd = hTaskbar;
        state_->tbWidth = tbWidth;
        state_->stability = stability;
    }
    worker_ = std::thread(UiaWorkerLoop, state_);
    Log("[UIA-WORKER] worker thread started\n");
}

bool TaskbarGeometry::LoadFreshUiaSnapshot(UiaSnapshot& out, int maxAgeMs) {
    if (!state_) return false;
    std::lock_guard<std::mutex> lk(state_->mtx);
    if (!state_->snap.valid) return false;
    const ULONGLONG age = ::GetTickCount64() - state_->snap.tickMs;
    if (age > static_cast<ULONGLONG>(maxAgeMs)) return false;
    out = state_->snap;
    return true;
}

bool TaskbarGeometry::GetChildRectsByEnum(HWND hTaskbar,
                                          RECT& tl, bool& fTL, RECT& tr, bool& fTR,
                                          RECT& rb, bool& fRB, int tbW) {
    if (!hTaskbar) hTaskbar = FindTaskbarHandle();
    if (!hTaskbar) { fTL = fTR = fRB = false; return false; }
    return EnumQueryChildRects(hTaskbar, tl, fTL, tr, fTR, rb, fRB, tbW);
}

bool TaskbarGeometry::EnumFindTaskList(HWND hTaskbar, int tbWidth, RECT& out) {
    if (!hTaskbar) hTaskbar = FindTaskbarHandle();
    if (!hTaskbar) return false;
    return EnumFindTaskListOnly(hTaskbar, tbWidth, out);
}

void TaskbarGeometry::ShutdownUiaWorker() {
    if (!state_) return;
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        state_->stop = true;
        state_->cv.notify_all();
    }
    // 工作线程可能正阻塞在无超时的 UIA 跨进程调用上，不能无限等待 join：
    // 分离之。线程函数只持有 state_ 的 shared_ptr 副本且不触碰 this，
    // 分离后完成当前调用即退出，COM 对象在其所属线程上安全释放。
    if (worker_.joinable()) worker_.detach();
    state_.reset();
}

} // namespace moekoe
