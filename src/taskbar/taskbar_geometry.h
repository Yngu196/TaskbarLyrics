// SPDX-License-Identifier: GPL-3.0
// taskbar_geometry.h - 任务栏几何信息与 UIA 枚举
//
// 职责:
//   - 任务栏尺寸/位置/DPI/AutoHide 检测
//   - UIA 子窗口枚举（后台工作线程 + 快照缓存）
//   - SetWinEventHook 监听任务栏变化
//   - TaskbarInfo 结构体
//
// 线程模型（重要）：
//   UIA 是无超时的跨进程 COM 调用，若在 UI 线程直接发起，可能与其他进程
//   （如设置应用 immersivecontrolpanel）形成双向等待死锁（WER MoAppHangXProc）。
//   因此所有 UIA 调用只在后台工作线程执行：
//     - UI 线程通过 RequestUiaRefresh 非阻塞请求枚举（内部节流）
//     - UI 线程通过 LoadFreshUiaSnapshot 读取快照（无锁等待，拿不到就降级）
//     - UI 线程兜底路径使用 EnumChildWindows（不发送跨进程消息，安全）
//
// 拆分自 taskbar_window.h/cpp (A1)
#pragma once

#include <windows.h>
#include <UIAutomation.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace moekoe {

enum class TaskbarPosition { BOTTOM, TOP, LEFT, RIGHT, UNKNOWN };

struct TaskbarInfo {
    RECT            rect{0, 0, 0, 0};
    TaskbarPosition position{TaskbarPosition::UNKNOWN};
    UINT            dpi{96};
    bool            autoHide{false};
};

// UIA 后台快照：由工作线程发布，UI 线程只读副本
struct UiaSnapshot {
    RECT taskList{}, tray{}, rebar{};
    bool hasTaskList{false}, hasTray{false}, hasRebar{false};

    // 稳定性二次采样（请求带 stability 标志时由工作线程完成）：
    // 与第一次采样相隔约 2ms，用于检测任务栏动画期间的布局抖动
    RECT tbCheck{};       bool tbCheckValid{false};
    RECT taskListCheck{}; bool taskListCheckValid{false};

    ULONGLONG tickMs{0};   // 快照完成时刻（GetTickCount64）
    bool valid{false};     // 本次枚举是否成功
};

// 工作线程共享状态：UI 线程与工作线程都只通过该结构交互，
// 线程函数仅捕获 shared_ptr 副本，分离后依然安全（不触碰所属对象）
struct UiaWorkerState {
    std::mutex mtx;
    std::condition_variable cv;
    bool stop{false};
    bool pending{false};
    HWND hwnd{nullptr};
    int  tbWidth{0};
    bool stability{false};
    UiaSnapshot snap;
};

class TaskbarGeometry {
public:
    TaskbarGeometry();
    ~TaskbarGeometry();

    TaskbarGeometry(const TaskbarGeometry&) = delete;
    TaskbarGeometry& operator=(const TaskbarGeometry&) = delete;

    // 查找 Shell_TrayWnd
    static HWND FindTaskbarHandle();

    // 获取任务栏信息：位置方位（基于 rcWork 中心线判断水平/垂直 + 偏左/偏右/偏上/偏下）、
    // DPI、AutoHide 状态（SHAppBarMessage ABS_AUTOHIDE）
    TaskbarInfo Detect(HWND hTaskbar);
    TaskbarInfo GetInfo() const { return info_; }

    // ── 后台 UIA 工作线程接口（UI 线程调用，全部非阻塞） ──
    // 非阻塞请求一次后台 UIA 枚举；intervalMs 内的重复请求被节流忽略。
    // stability=true 时工作线程附带 2ms 间隔的二次采样（帧锁定用）。
    void RequestUiaRefresh(HWND hTaskbar, int tbWidth, bool stability,
                           int intervalMs = 200);
    // 读取 maxAgeMs 内的快照副本；无快照/过期返回 false（调用方应走枚举兜底）
    bool LoadFreshUiaSnapshot(UiaSnapshot& out, int maxAgeMs);
    // EnumChildWindows 枚举兜底（GetClassNameW/GetWindowRect 不发送跨进程消息，
    // UI 线程可安全调用；Win11 任务栏为虚拟化子窗口时可能查不到关键区域）
    bool GetChildRectsByEnum(HWND hTaskbar,
                             RECT& taskListRect, bool& foundTaskList,
                             RECT& trayRect,    bool& foundTray,
                             RECT& rebarRect,   bool& foundRebar,
                             int   tbWidth);
    // 仅查找任务列表子窗口（帧锁定二次采样的枚举兜底用）
    bool EnumFindTaskList(HWND hTaskbar, int tbWidth, RECT& out);
    // 停止工作线程。工作线程可能正阻塞在无超时的 UIA 调用上，
    // 因此不等待 join：分离线程，状态由 shared_ptr 保活（线程不触碰 this）
    void ShutdownUiaWorker();

    // 自动隐藏状态
    bool IsAutoHide() const { return info_.autoHide; }
    void SetAutoHideVisible(bool v) { taskbarVisible_ = v; }
    bool IsAutoHideVisible() const { return taskbarVisible_; }

    // 方位判断
    bool IsVertical() const {
        return info_.position == TaskbarPosition::LEFT ||
               info_.position == TaskbarPosition::RIGHT;
    }

private:
    TaskbarInfo info_{};
    bool        taskbarVisible_{true};

    std::shared_ptr<UiaWorkerState> state_{};
    std::thread   worker_{};
    ULONGLONG     lastRequestTick_{0};   // 请求节流（仅 UI 线程访问）
};

} // namespace moekoe
