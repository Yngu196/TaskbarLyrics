// SPDX-License-Identifier: GPL-3.0
// app_context.cpp - 应用上下文辅助函数实现
//
// 从 main.cpp 提取：
//   - ToTooltipWide / ApplyRendererSettings（通用辅助）
//   - HandleFrameTick（帧渲染流程，原 MsgWndProc WM_TIMER 分支）
//   - HandleRenderUpdate（悬停重绘，原 MsgWndProc WM_RENDER_UPDATE 分支）
//
#include "core/app_context.h"

#include "core/constants.h"
#include "util/logger.h"

#include <psapi.h>

#pragma comment(lib, "psapi.lib")

#include <algorithm>
#include <cmath>

namespace moekoe {

using namespace moekoe::constants;

// 日志快捷方式（使用统一日志系统）
using moekoe::Log;
using moekoe::LogError;

// 工具: 把 UTF-8 字符串限制到 Windows Tooltip 长度
std::wstring ToTooltipWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    if (len > WINDOWS_TOOLTIP_MAX_LEN) len = WINDOWS_TOOLTIP_MAX_LEN;
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len);
    return out;
}

// 应用渲染器配置（直接传递 AppearanceConfig，无需逐字段拷贝）
void ApplyRendererSettings(AppContext& app) {
    if (!app.renderer || !app.config) return;
    Log("[CONFIG] ApplyRenderer: hl=%s nl=%s font=%s size=%d opacity=%.2f\n",
        app.config->Appearance().highlightColor.c_str(), app.config->Appearance().normalColor.c_str(),
        app.config->Appearance().fontFamily.c_str(), app.config->Appearance().fontSize,
        app.config->Appearance().normalOpacity);
    app.renderer->ApplySettings(app.config->Appearance());
}

// 帧定时器处理：从 parser 取渲染状态并驱动 renderer
// 从 MsgWndProc 的 WM_TIMER / WM_FRAME_TICK 分支提取
void HandleFrameTick(AppContext& app) {
    try {
        // ═══════ 性能监控：帧计数累加 ═══════
        // 仅在 debug 模式下累加（避免非 debug 模式下无意义的原子操作）
        if (app.config && app.config->Advanced().debugLog) {
            ++app.perfFrameCount_;
        }

        // ═══════ 帧渲染流程 ═══════
        // 1. 检测任务栏尺寸变化（含 APPBAR 自动隐藏鼠标进出检测）
        if (app.taskbarWindow) app.taskbarWindow->CheckResize();

        // 2. APPBAR 自动隐藏：窗口已隐藏时跳过渲染（避免 UpdateLayeredWindow 隐式显示导致闪烁）
        if (app.taskbarWindow && app.taskbarWindow->IsAutoHideHidden()) return;

        // 2.5. 全屏检测防抖：由 FullscreenDetector 独立处理
        if (app.taskbarWindow && app.config && app.config->Advanced().enableFullscreenHide) {
            bool shouldHide = false;
            if (app.taskbarWindow->Fullscreen().Update(
                    app.config->Advanced().enableFullscreenHide,
                    app.config->Advanced().debugLog,
                    shouldHide)) {
                app.taskbarWindow->SetFullscreenHidden(shouldHide);
                if (!shouldHide) {
                    // 退出全屏恢复时重定位，确保使用用户拖动的 dragOffset 而非清零
                    app.taskbarWindow->InvalidatePositionCache();
                    app.taskbarWindow->Reposition();
                }
            }

            // Shell 交互即时恢复：消费 forceDebounceReset 标志
            if (app.taskbarWindow->Fullscreen().ConsumeForceDebounceReset()) {
                if (app.config->Advanced().debugLog)
                    Log("[FullscreenDetect] debounce reset by external trigger (shell interaction)\n");
            }
        }

        // 2.6. 全屏隐藏时跳过渲染（与 APPBAR 同理）
        if (app.taskbarWindow && app.taskbarWindow->IsFullscreenHidden()) return;

        // 3. 从歌词解析器获取当前应渲染的状态
        if (app.parser && app.renderer) {
            auto state = app.parser->GetCurrentRenderState();

            // 3.1 集成频谱数据（纯音乐播放时使用）
            if (app.spectrumCapture && app.spectrumCapture->IsRunning()) {
                state.spectrumBands = app.spectrumCapture->GetSpectrum(SPECTRUM_NUM_BANDS);
            }

            // 3. 附加 UI 状态（悬停/拖动，用于判断是否显示控制按钮）
            if (app.taskbarWindow) {
                state.isHovering = app.taskbarWindow->IsHovering();
                state.isDragging = app.taskbarWindow->IsDragging();
            }

            // 4. 同步任务栏方向（运行时位置变化适配）+ 执行渲染
            app.renderer->SetVerticalTaskbar(app.taskbarWindow->IsVerticalTaskbar());
            app.renderer->Render(state);

            // 4.5. 卡片模式动态宽度：长歌词扩展，短歌词滞回缩回
            if (app.config->Appearance().displayMode == "card" &&
                app.config->Appearance().cardDynamicWidth &&
                state.hasLyrics && !state.currentLine.empty()) {
                const float newWidthDip = app.renderer->MeasureCardLyricsWidth(
                    state.currentLine, state.nextLine);
                const int newWidthInt = static_cast<int>(std::ceil(newWidthDip));
                const int currentDip = app.taskbarWindow->GetDynamicCardWidthDip();

                // 扩展：测量值更大时立即生效
                if (newWidthInt > currentDip) {
                    app.taskbarWindow->SetDynamicCardWidthDip(newWidthInt);
                    app.taskbarWindow->Reposition();
                    app.shrinkStartTick_ = 0;
                }
                // 缩回：测量值 ≤ 默认宽度（360 DIPs）且当前处于扩展状态
                // 连续 2 秒无长歌词后才缩回，避免短暂切换导致抖动
                else if (newWidthInt <= CARD_MIN_WIDTH_BASE_DP * 2 &&
                         currentDip > CARD_MIN_WIDTH_BASE_DP * 2) {
                    if (app.shrinkStartTick_ == 0) {
                        app.shrinkStartTick_ = ::GetTickCount64();
                    } else if (::GetTickCount64() - app.shrinkStartTick_ >= 2000) {
                        app.taskbarWindow->SetDynamicCardWidthDip(0);
                        app.taskbarWindow->Reposition();
                        app.shrinkStartTick_ = 0;
                    }
                } else {
                    app.shrinkStartTick_ = 0;
                }
            }

            // 5. 更新托盘提示文本（实时显示当前歌词）
            if (app.tray && state.hasLyrics) {
                auto tip = ToTooltipWide(state.currentLine);
                if (!tip.empty()) {
                    app.tray->SetTooltip(tip);
                }
            }
        }

        // ═══════ 6. 性能监控：定期汇总输出到日志 ═══════
        // 仅在 debug 模式下启用，每 PERF_STATS_INTERVAL_MS 毫秒输出一次
        //   FPS=区间内帧数 / 区间秒数
        //   wsMsgRate=区间内 WS 消息数 / 区间秒数
        //   wsConn=当前 WebSocket 连接状态
        //   mem=进程工作集大小（MB）
        if (app.config && app.config->Advanced().debugLog) {
            const ULONGLONG nowTick = ::GetTickCount64();
            if (app.perfLastTick_ == 0) {
                app.perfLastTick_ = nowTick;
            } else if (nowTick - app.perfLastTick_ >= static_cast<ULONGLONG>(PERF_STATS_INTERVAL_MS)) {
                const double elapsedSec = static_cast<double>(nowTick - app.perfLastTick_) / 1000.0;
                const double fps = static_cast<double>(app.perfFrameCount_) / elapsedSec;
                const UINT wsMsgs = app.perfWsMsgCount_.exchange(0, std::memory_order_relaxed);
                const double wsMsgRate = static_cast<double>(wsMsgs) / elapsedSec;
                const int wsConn = app.wsClient ? (app.wsClient->IsConnected() ? 1 : 0) : -1;

                // 工作集（Working Set）大小，反映进程实际占用的物理内存
                double memMB = 0.0;
                PROCESS_MEMORY_COUNTERS pmc{};
                if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) {
                    memMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
                }

                Log("[PERF] FPS=%.1f wsMsgRate=%.1f/s wsConn=%d mem=%.1fMB\n",
                    fps, wsMsgRate, wsConn, memMB);

                // 重置统计窗口
                app.perfFrameCount_ = 0;
                app.perfLastTick_ = nowTick;
            }
        }
    } catch (const std::exception& e) {
        Log("[CRASH] WM_TIMER exception: %s\n", e.what());
        // 恢复策略：尝试重置渲染器状态
        if (app.renderer) {
            try {
                app.renderer->Shutdown();
                if (app.taskbarWindow)
                    app.renderer->Initialize(app.taskbarWindow->GetHandle());
                Log("[RECOVER] Renderer reset succeeded\n");
            } catch (...) {
                Log("[FATAL] Renderer recovery failed, shutting down\n");
                app.running = false;
                ::PostQuitMessage(0);
            }
        }
    } catch (...) {
        Log("[CRASH] WM_TIMER unknown exception, shutting down\n");
        app.running = false;
        ::PostQuitMessage(0);
    }
}

// 悬停/拖动重绘请求处理（WM_RENDER_UPDATE）
void HandleRenderUpdate(AppContext& app) {
    try {
        // APPBAR 自动隐藏时跳过悬停重绘
        if (app.taskbarWindow && app.taskbarWindow->IsAutoHideHidden()) return;

        // 全屏隐藏时跳过悬停重绘
        if (app.taskbarWindow && app.taskbarWindow->IsFullscreenHidden()) return;

        if (app.parser && app.renderer && app.taskbarWindow) {
            auto state = app.parser->GetCurrentRenderState();
            state.isHovering = app.taskbarWindow->IsHovering();
            state.isDragging = app.taskbarWindow->IsDragging();
            app.renderer->Render(state);
        }
    } catch (const std::exception& e) {
        LogError("[RENDER_UPDATE] exception: %s\n", e.what());
    } catch (...) {
        LogError("[RENDER_UPDATE] unknown exception suppressed\n");
    }
}

} // namespace moekoe
