// SPDX-License-Identifier: GPL-2.0
// taskbar_geometry.cpp - 任务栏几何信息与 UIA 枚举实现
//
// 任务栏方位判断算法：
//   - 水平任务栏（BOTTOM / TOP）：tbWidth > tbHeight，再按 tb.top 与 rcWork 中心线比大小
//   - 垂直任务栏（LEFT / RIGHT）：tbWidth ≤ tbHeight，按 tb.left 与 rcWork 中心线比大小
// UIA 枚举：通过 IUIAutomation::ElementFromHandle 获取 Shell_TrayWnd 的 UIA 元素树，
//   按 ClassName 筛选子元素（MSTaskListWClass / TrayNotifyWnd / ReBarWindow32），
//   提取 BoundingRectangle。UIA 不可用时降级到 EnumChildWindows。
#include "taskbar_geometry.h"

#include <shellapi.h>
#include <windows.h>

namespace moekoe {

TaskbarGeometry::TaskbarGeometry() = default;
TaskbarGeometry::~TaskbarGeometry() { CleanupUIA(); }

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

void TaskbarGeometry::InitUIA() {
    if (uia_) return;
    ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia_));
}

void TaskbarGeometry::CleanupUIA() {
    if (uia_) { uia_->Release(); uia_ = nullptr; }
}

bool TaskbarGeometry::GetChildRectsByUIA(RECT& tl, bool& fTL, RECT& tr, bool& fTR,
                                          RECT& rb, bool& fRB, int tbW) {
    fTL = fTR = fRB = false;
    if (!uia_) {
        // ── 降级：UIA 不可用（COM 未初始化或 CoCreateInstance 失败），回退到 EnumChildWindows ──
        // Win11 任务栏子窗口结构：
        //   MSTaskListWClass              → 任务列表（应用图标区），需要右侧 edge 来计算可用宽度
        //   DesktopWindowContentBridge    → Win11 24H2+ 任务列表的替代类名（非标准）
        //   TrayNotifyWnd                 → 系统托盘（时钟、通知图标）
        //   ReBarWindow32                 → 工具栏容器
        HWND hTB = ::FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!hTB) return false;
        HWND hChild = ::GetWindow(hTB, GW_CHILD);
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
        return true;  // 降级成功
    }

    // ── UIA 路径：通过 IUIAutomationElement 枚举 Shell_TrayWnd 子元素 ──
    IUIAutomationElement* tbElem = nullptr;
    HWND hTB = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (FAILED(uia_->ElementFromHandle(hTB, &tbElem)) || !tbElem) return false;

    // 辅助 lambda：按 ClassName 查找子元素，提取其 CurrentBoundingRectangle
    // maxW 用于 DesktopWindowContentBridge 去重（区分任务列表和托盘包装器）
    auto findClass = [&](const wchar_t* cls, RECT& out, bool& found, int maxW = 0) {
        VARIANT v; v.vt = VT_BSTR; v.bstrVal = ::SysAllocString(cls);
        IUIAutomationCondition* c = nullptr;
        if (SUCCEEDED(uia_->CreatePropertyCondition(UIA_ClassNamePropertyId, v, &c)) && c) {
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

bool TaskbarGeometry::IsUiaCacheExpired(int intervalMs) {
    if (!cachedUiaValid_) return true;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - lastUiaRefreshTime_).count() >= intervalMs;
}

void TaskbarGeometry::CacheUiaResults(RECT tl, bool hTL, RECT tr, bool hTR, RECT rb, bool hRB) {
    cachedTaskListRect_ = tl; cachedTrayRect_ = tr; cachedRebarRect_ = rb;
    cachedTaskListValid_ = hTL; cachedTrayValid_ = hTR; cachedRebarValid_ = hRB;
    cachedUiaValid_ = true; lastUiaRefreshTime_ = std::chrono::steady_clock::now();
}

void TaskbarGeometry::LoadUiaCache(RECT& tl, bool& hTL, RECT& tr, bool& hTR, RECT& rb, bool& hRB) {
    tl = cachedTaskListRect_; tr = cachedTrayRect_; rb = cachedRebarRect_;
    hTL = cachedTaskListValid_; hTR = cachedTrayValid_; hRB = cachedRebarValid_;
}

} // namespace moekoe
