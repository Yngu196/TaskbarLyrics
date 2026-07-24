// SPDX-License-Identifier: GPL-3.0
// config_dialog.cpp - GUI 配置界面实现
//
// 使用 Win32 对话框实现配置界面，不依赖资源文件
// 所有控件在 OnInitDialog 中动态创建
#include "ui/config_dialog.h"

#include <commdlg.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace moekoe {

// 控件 ID
enum {
    IDC_TAB_MAIN = 100,
    // 外观页
    IDC_LABEL_HLCOLOR = 200,
    IDC_EDIT_HLCOLOR,
    IDC_BTN_HLCOLOR,
    IDC_LABEL_NMCOLOR,
    IDC_EDIT_NMCOLOR,
    IDC_BTN_NMCOLOR,
    IDC_LABEL_OPACITY,
    IDC_EDIT_OPACITY,
    IDC_SPIN_OPACITY,
    IDC_LABEL_FONT,
    IDC_EDIT_FONT,
    IDC_BTN_FONT,
    IDC_LABEL_FONTSIZE,
    IDC_EDIT_FONTSIZE,
    IDC_SPIN_FONTSIZE,
    IDC_CHK_KARAOKE,
    IDC_CHK_TRANSLATION,
    // 高级页
    IDC_LABEL_PORT,
    IDC_EDIT_PORT,
    IDC_LABEL_RATE,
    IDC_EDIT_RATE,
    IDC_SPIN_RATE,
    // 绑定模式
    IDC_LABEL_BOUND,
    IDC_BTN_UNBIND,
    // 按钮
    IDC_BTN_OK = 500,
    IDC_BTN_CANCEL,
    IDC_BTN_APPLY,
};

namespace {

// 线程局部回调（由 Show 设置，DialogProc 使用）
std::function<void()>& GetOnUnbind() {
    static std::function<void()> s_onUnbind;
    return s_onUnbind;
}

// 线程局部：对话框结果（OK/Apply = true）
bool& GetDialogResult() {
    static bool s_result = false;
    return s_result;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(),
                          static_cast<int>(s.size()), &out[0], len, nullptr, nullptr);
    return out;
}

// 创建标签
HWND CreateLabel(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return ::CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
}

// 创建编辑框
HWND CreateEdit(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
}

// 创建数字编辑框（仅数字）
HWND CreateNumEdit(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
}

// 创建按钮
HWND CreateButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    return ::CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
}

// 创建复选框
HWND CreateCheckBox(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, bool checked) {
    HWND hw = ::CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
    if (checked) ::SendMessageW(hw, BM_SETCHECK, BST_CHECKED, 0);
    return hw;
}

// 设置编辑框文本
void SetEditText(HWND hwnd, int id, const wchar_t* text) {
    ::SetDlgItemTextW(hwnd, id, text);
}

// 获取编辑框文本
std::wstring GetEditText(HWND hwnd, int id) {
    wchar_t buf[256] = {};
    ::GetDlgItemTextW(hwnd, id, buf, 256);
    return buf;
}

} // namespace

bool ConfigDialog::Show(HINSTANCE hInstance, HWND parent, Config& config,
                        bool boundMode, std::function<void()> onUnbind) {
    // 使用自定义窗口类创建模态对话框
    // 通过 DialogBoxParam 需要资源，这里用 CreateWindow 模拟模态对话框

    // 注册对话框窗口类
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = &DefWindowProcW; // 临时，后续子类化
        wc.hInstance     = hInstance;
        wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"MoeKoeConfigDialog";
        ::RegisterClassExW(&wc);
        registered = true;
    }

    // 保存配置指针到线程局部
    static Config* s_config = nullptr;
    s_config = &config;

    // 保存 onUnbind 回调
    GetOnUnbind() = std::move(onUnbind);

    // 重置对话框结果
    GetDialogResult() = false;

    // 创建窗口（可调整大小，标题栏可拖动）
    HWND hwnd = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        L"MoeKoeConfigDialog",
        L"MoeKoe Taskbar Lyrics - 设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 520,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) return false;

    // 子类化窗口过程
    ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DialogProc));
    // 保存 config 指针
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s_config));

    InitControls(hwnd, config, boundMode);

    // 居中显示（相对屏幕居中，因为父窗口可能是 message-only 窗口）
    RECT rcDlg{};
    ::GetWindowRect(hwnd, &rcDlg);
    int screenW = ::GetSystemMetrics(SM_CXSCREEN);
    int screenH = ::GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - (rcDlg.right - rcDlg.left)) / 2;
    int y = (screenH - (rcDlg.bottom - rcDlg.top)) / 2;
    ::SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);

    // 模态消息循环（不能用 PostQuitMessage，否则会杀死主程序）
    bool dialogActive = true;
    MSG msg{};
    while (dialogActive && ::IsWindow(hwnd) && ::GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            ::DestroyWindow(hwnd);
            break;
        }
        // 过滤掉 WM_QUIT，防止主程序意外退出
        if (msg.message == WM_QUIT) {
            // 把 WM_QUIT 放回队列，留给主消息循环处理
            ::PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ::EnableWindow(parent, TRUE);
    ::SetForegroundWindow(parent);

    return GetDialogResult();
}

void ConfigDialog::InitControls(HWND hwnd, const Config& config, bool boundMode) {
    const int labelW = 90, editW = 160, btnW = 50, numW = 60;
    const int left = 20, top = 18, rowH = 30, gap = 8;
    int y = top;

    // === 外观设置 ===
    CreateLabel(hwnd, 0, L"外观设置", left, y, 200, 20);
    y += rowH;

    // 高亮颜色
    CreateLabel(hwnd, IDC_LABEL_HLCOLOR, L"高亮颜色:", left, y + 3, labelW, 20);
    CreateEdit(hwnd, IDC_EDIT_HLCOLOR, Utf8ToWide(config.Appearance().highlightColor).c_str(), left + labelW + gap, y, editW, 24);
    CreateButton(hwnd, IDC_BTN_HLCOLOR, L"...", left + labelW + gap + editW + gap, y, btnW, 24);
    y += rowH;

    // 普通颜色
    CreateLabel(hwnd, IDC_LABEL_NMCOLOR, L"普通颜色:", left, y + 3, labelW, 20);
    CreateEdit(hwnd, IDC_EDIT_NMCOLOR, Utf8ToWide(config.Appearance().normalColor).c_str(), left + labelW + gap, y, editW, 24);
    CreateButton(hwnd, IDC_BTN_NMCOLOR, L"...", left + labelW + gap + editW + gap, y, btnW, 24);
    y += rowH;

    // 普通透明度
    CreateLabel(hwnd, IDC_LABEL_OPACITY, L"普通透明度:", left, y + 3, labelW, 20);
    wchar_t opacityBuf[16] = {};
    _snwprintf_s(opacityBuf, _TRUNCATE, L"%.0f", config.Appearance().normalOpacity * 100);
    CreateNumEdit(hwnd, IDC_EDIT_OPACITY, opacityBuf, left + labelW + gap, y, numW, 24);
    CreateLabel(hwnd, 0, L"%", left + labelW + gap + numW + 4, y + 3, 20, 20);
    y += rowH;

    // 字体
    CreateLabel(hwnd, IDC_LABEL_FONT, L"字体:", left, y + 3, labelW, 20);
    CreateEdit(hwnd, IDC_EDIT_FONT, Utf8ToWide(config.Appearance().fontFamily).c_str(), left + labelW + gap, y, editW, 24);
    CreateButton(hwnd, IDC_BTN_FONT, L"...", left + labelW + gap + editW + gap, y, btnW, 24);
    y += rowH;

    // 字号
    CreateLabel(hwnd, IDC_LABEL_FONTSIZE, L"字号:", left, y + 3, labelW, 20);
    wchar_t fontSizeBuf[16] = {};
    _snwprintf_s(fontSizeBuf, _TRUNCATE, L"%d", config.Appearance().fontSize);
    CreateNumEdit(hwnd, IDC_EDIT_FONTSIZE, fontSizeBuf, left + labelW + gap, y, numW, 24);
    CreateLabel(hwnd, 0, L"pt", left + labelW + gap + numW + 4, y + 3, 20, 20);
    y += rowH;

    // 卡拉OK
    CreateCheckBox(hwnd, IDC_CHK_KARAOKE, L"启用卡拉OK逐字高亮", left, y, 250, 22, config.Appearance().enableKaraoke);
    y += rowH;

    // 翻译
    CreateCheckBox(hwnd, IDC_CHK_TRANSLATION, L"显示翻译歌词", left, y, 250, 22, config.Appearance().enableTranslation);
    y += rowH + 8;

    // === 高级设置 ===
    CreateLabel(hwnd, 0, L"高级设置", left, y, 200, 20);
    y += rowH;

    // WebSocket 端口
    CreateLabel(hwnd, IDC_LABEL_PORT, L"WebSocket 端口:", left, y + 3, labelW + 20, 20);
    wchar_t portBuf[16] = {};
    _snwprintf_s(portBuf, _TRUNCATE, L"%d", config.Advanced().websocketPort);
    CreateNumEdit(hwnd, IDC_EDIT_PORT, portBuf, left + labelW + 20 + gap, y, numW + 20, 24);
    y += rowH;

    // 刷新率
    CreateLabel(hwnd, IDC_LABEL_RATE, L"刷新率:", left, y + 3, labelW + 20, 20);
    wchar_t rateBuf[16] = {};
    _snwprintf_s(rateBuf, _TRUNCATE, L"%d", config.Advanced().refreshRateHz);
    CreateNumEdit(hwnd, IDC_EDIT_RATE, rateBuf, left + labelW + 20 + gap, y, numW + 20, 24);
    CreateLabel(hwnd, 0, L"FPS", left + labelW + 20 + gap + numW + 24, y + 3, 30, 20);
    y += rowH + 8;

    // === 绑定模式 ===
    CreateLabel(hwnd, 0, L"运行模式", left, y, 200, 20);
    y += rowH;

    if (boundMode) {
        CreateLabel(hwnd, IDC_LABEL_BOUND, L"当前: 绑定模式（随 MoeKoeMusic 启停）", left, y + 3, 300, 20);
        y += rowH;
        CreateButton(hwnd, IDC_BTN_UNBIND, L"解除绑定", left, y, 100, 26);
        y += rowH + 8;
    } else {
        CreateLabel(hwnd, IDC_LABEL_BOUND, L"当前: 独立模式（常驻系统托盘）", left, y + 3, 300, 20);
        y += rowH + 8;
    }

    // === 按钮 ===
    int btnY = y;
    int totalBtnW = 80 * 3 + 10 * 2;
    int btnStartX = (420 - totalBtnW) / 2;
    CreateButton(hwnd, IDC_BTN_OK, L"确定", btnStartX, btnY, 80, 28);
    CreateButton(hwnd, IDC_BTN_CANCEL, L"取消", btnStartX + 90, btnY, 80, 28);
    CreateButton(hwnd, IDC_BTN_APPLY, L"应用", btnStartX + 180, btnY, 80, 28);
}

bool ConfigDialog::ReadControls(HWND hwnd, Config& config) {
    auto hlColor = WideToUtf8(GetEditText(hwnd, IDC_EDIT_HLCOLOR));
    auto nmColor = WideToUtf8(GetEditText(hwnd, IDC_EDIT_NMCOLOR));
    auto opacityStr = GetEditText(hwnd, IDC_EDIT_OPACITY);
    auto fontFamily = WideToUtf8(GetEditText(hwnd, IDC_EDIT_FONT));
    auto fontSizeStr = GetEditText(hwnd, IDC_EDIT_FONTSIZE);
    auto portStr = GetEditText(hwnd, IDC_EDIT_PORT);
    auto rateStr = GetEditText(hwnd, IDC_EDIT_RATE);

    auto& a = config.MutableAppearance();
    a.highlightColor    = hlColor.empty() ? a.highlightColor : hlColor;
    a.normalColor       = nmColor.empty() ? a.normalColor : nmColor;
    a.normalOpacity     = opacityStr.empty() ? a.normalOpacity : std::clamp(_wtoi(opacityStr.c_str()) / 100.0, 0.0, 1.0);
    a.fontFamily        = fontFamily.empty() ? a.fontFamily : fontFamily;
    a.fontSize          = fontSizeStr.empty() ? a.fontSize : std::max(8, _wtoi(fontSizeStr.c_str()));
    a.enableKaraoke     = ::IsDlgButtonChecked(hwnd, IDC_CHK_KARAOKE) == BST_CHECKED;
    a.enableTranslation = ::IsDlgButtonChecked(hwnd, IDC_CHK_TRANSLATION) == BST_CHECKED;

    auto& adv = config.MutableAdvanced();
    adv.websocketPort = portStr.empty() ? adv.websocketPort : std::clamp(_wtoi(portStr.c_str()), 1024, 65535);
    adv.refreshRateHz = rateStr.empty() ? adv.refreshRateHz : std::clamp(_wtoi(rateStr.c_str()), 10, 120);

    return config.Save();
}

bool ConfigDialog::PickColor(HWND hwnd, HWND colorEdit) {
    static DWORD customColors[16] = {};

    wchar_t buf[32] = {};
    ::GetWindowTextW(colorEdit, buf, 32);
    std::wstring hexStr = buf;
    COLORREF initial = RGB(255, 255, 255);
    if (hexStr.size() >= 7 && hexStr[0] == L'#') {
        unsigned int r = 0, g = 0, b = 0;
        swscanf_s(hexStr.c_str() + 1, L"%02x%02x%02x", &r, &g, &b);
        initial = RGB(r, g, b);
    }

    CHOOSECOLORW cc{};
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = hwnd;
    cc.rgbResult    = initial;
    cc.lpCustColors = customColors;
    cc.Flags        = CC_FULLOPEN | CC_RGBINIT;

    if (::ChooseColorW(&cc)) {
        wchar_t newHex[16] = {};
        _snwprintf_s(newHex, _TRUNCATE, L"#%02X%02X%02X",
                     GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
        ::SetWindowTextW(colorEdit, newHex);
        return true;
    }
    return false;
}

bool ConfigDialog::PickFont(HWND hwnd, HWND fontEdit) {
    wchar_t currentFont[256] = {};
    ::GetWindowTextW(fontEdit, currentFont, 256);

    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, currentFont);
    lf.lfHeight = -14;

    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner   = hwnd;
    cf.lpLogFont   = &lf;
    cf.Flags       = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;

    if (::ChooseFontW(&cf)) {
        ::SetWindowTextW(fontEdit, lf.lfFaceName);
        return true;
    }
    return false;
}

INT_PTR CALLBACK ConfigDialog::DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Config* config = reinterpret_cast<Config*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        // int code = HIWORD(wParam);

        switch (id) {
        case IDC_BTN_HLCOLOR: {
            HWND edit = ::GetDlgItem(hwnd, IDC_EDIT_HLCOLOR);
            PickColor(hwnd, edit);
            return TRUE;
        }
        case IDC_BTN_NMCOLOR: {
            HWND edit = ::GetDlgItem(hwnd, IDC_EDIT_NMCOLOR);
            PickColor(hwnd, edit);
            return TRUE;
        }
        case IDC_BTN_FONT: {
            HWND edit = ::GetDlgItem(hwnd, IDC_EDIT_FONT);
            PickFont(hwnd, edit);
            return TRUE;
        }
        case IDC_BTN_OK: {
            if (config && ReadControls(hwnd, *config)) {
                GetDialogResult() = true;
                ::DestroyWindow(hwnd);
            } else {
                ::MessageBoxW(hwnd, L"保存配置失败", L"错误", MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        case IDC_BTN_CANCEL: {
            ::DestroyWindow(hwnd);
            return TRUE;
        }
        case IDC_BTN_APPLY: {
            if (config) {
                ReadControls(hwnd, *config);
                GetDialogResult() = true; // 让调用者知道需要应用设置
            }
            return TRUE;
        }
        case IDC_BTN_UNBIND: {
            int ret = ::MessageBoxW(hwnd,
                L"解除绑定后将退出本程序。\n\n下次启动请以独立模式运行（不放在 MoeKoeMusic 目录下）。\n\n确定要解除绑定吗？",
                L"解除绑定", MB_YESNO | MB_ICONQUESTION);
            if (ret == IDYES) {
                // 调用外部回调执行退出逻辑
                if (GetOnUnbind()) GetOnUnbind()();
                ::DestroyWindow(hwnd);
            }
            return TRUE;
        }
        }
        break;
    }
    case WM_CLOSE: {
        ::DestroyWindow(hwnd);
        return TRUE;
    }
    case WM_DESTROY: {
        // 不用 PostQuitMessage！只需结束模态循环
        // IsWindow(hwnd) 在 DestroyWindow 后返回 FALSE，循环自然退出
        return TRUE;
    }
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace moekoe
