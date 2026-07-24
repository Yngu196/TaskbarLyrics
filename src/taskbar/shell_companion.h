// SPDX-License-Identifier: GPL-3.0
// shell_companion.h - Shell 层抽象：封装任务栏几何、窗口嵌入、全屏检测与 WinEvent 钩子
//
// 职责:
//   - 统一管理 Shell_TrayWnd 交互（几何检测、UIA 枚举、AutoHide、全屏隐藏）
//   - 封装 SetWinEventHook 基础设施（任务栏位置监听、Start Menu 冻结锁、前台检测）
//   - 提供干净查询接口给 TaskbarWindow / main.cpp
//
// v0.5.x: 从 taskbar_window.h/cpp 中抽取
#pragma once

#include "taskbar/fullscreen_detector.h"
#include "taskbar/taskbar_embedder.h"
#include "taskbar/taskbar_geometry.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace moekoe {

class ShellCompanion {
public:
    ShellCompanion();
    ~ShellCompanion();

    ShellCompanion(const ShellCompanion&) = delete;
    ShellCompanion& operator=(const ShellCompanion&) = delete;

    // ── 生命周期 ──
    // 初始化：查找任务栏、创建 embedder 窗口、启动 WinEvent 钩子
    bool Initialize(HWND hTaskbar, HWND lyricsWnd);
    void Shutdown();

    // ── 核心查询接口 ──
    RECT GetTaskbarRect() const;
    TaskbarInfo GetTaskbarInfo() const { return info_; }
    bool IsFullscreenHidden() const { return embedder_.IsFullscreenHidden(); }
    bool IsAutoHideHidden()  const { return taskbarAutoHide_ && !taskbarVisible_; }
    bool IsVerticalTaskbar() const {
        return info_.position == TaskbarPosition::LEFT ||
               info_.position == TaskbarPosition::RIGHT;
    }

    // ── 全屏显隐控制（含 Shell 锁定释放 / ForceReset / 定位恢复） ──
    // 调用 embedder_.SetFullscreenHidden(hidden) 执行实际的 SetWindowPos 操作
    void SetFullscreenHidden(bool hidden, HWND lyricsWnd);

    // ── 帧级更新（替代原 TaskbarWindow::CheckResize） ──
    // 处理全屏检测 + auto-hide 状态 + 任务栏尺寸变化
    // 返回 true 表示检测到布局变化需要 reposition
    void CheckResize(HWND lyricsWnd);

    // ── 定位计算 + SetWindowPos ──
    // 根据任务栏方位、UIA 子窗口、显示模式计算歌词窗口坐标并定位
    // inOutLastPosRect: SetWindowPos 短路优化的坐标缓存（调用方维护）
    void PositionLyricsInTaskbar(HWND lyricsWnd,
                                  const std::string& displayMode,
                                  int dragOffsetX, int dragOffsetY,
                                  RECT& inOutLastPosRect,
                                  int dynamicWidthDip = 0);

    // ── 拖动后吸附到空闲空间 ──
    void SnapToEmptySpace(HWND lyricsWnd);

    // ── Shell 交互锁定管理（供 WndProc 定时器回调） ──
    // timerId: 3 = MENUPOPUPEND, 4 = ForegroundHook 离开 Start Menu
    void UnlockShellInteraction(int timerId);

    // ── 子模块访问（供外部需要深入控制的代码） ──
    TaskbarGeometry&    Geometry()  { return geometry_; }
    FullscreenDetector& Fullscreen() { return fullscreenDetector_; }
    TaskbarEmbedder&    Embedder()  { return embedder_; }

    // ── 回调 ──
    using RepositionCallback = std::function<void()>;
    void OnRepositionNeeded(RepositionCallback cb);
    void RequestReposition(HWND lyricsWnd);  // Post WM_DELAYED_REPOSITION

    // ── 任务栏句柄访问 ──
    HWND GetTaskbarHandle() const { return hTaskbar_; }
    void RefreshTaskbarHandle(HWND hNew) {
        hTaskbar_ = hNew;
        ::OutputDebugStringW(L"[ShellCompanion] Taskbar handle refreshed\n");
    }

    // ── 静态：查找任务栏句柄 ──
    static HWND FindTaskbarHandle();

private:
    // ── 内部方法 ──
    void DetectTaskbarInfo();

    // ── WinEvent 钩子基础设施 ──
    void InstallHooks(HWND lyricsWnd);
    void RemoveHooks();

    static void CALLBACK TaskbarWinEventProc(
        HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static void CALLBACK ShellMenuWinEventProc(
        HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static void CALLBACK ForegroundWinEventProc(
        HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    // ── 成员 ──
    TaskbarGeometry    geometry_;
    TaskbarEmbedder    embedder_;
    FullscreenDetector fullscreenDetector_;

    HWND        hTaskbar_{nullptr};
    TaskbarInfo info_{};
    bool        taskbarAutoHide_{false};
    bool        taskbarVisible_{false};
    RECT        lastTaskbarRect_{0, 0, 0, 0};

    // 稳定性跟踪（帧锁定双采样）
    RECT  stableTaskbarRect_{};
    RECT  stableTaskListRect_{};
    bool  stableTaskListValid_{false};
    int   cachedRightEdgeOffset_{0};

    RepositionCallback onReposition_;

    // ── 静态：WinEvent 钩子状态 ──
    static ShellCompanion* s_instance_;
    static HWND s_lyricsWnd_;
    static HWINEVENTHOOK s_taskbarHook_;
    static HWINEVENTHOOK s_shellMenuHook_;
    static HWINEVENTHOOK s_foregroundHook_;
    static bool s_shellInteractionLocked_;
    static std::chrono::steady_clock::time_point s_shellInteractionLockedTime_;
    static RECT s_frozenTaskbarRect_;
    static RECT s_lastGoodTaskbarRect_;
    static bool s_lockedByStartMenuFg_;
};

} // namespace moekoe
