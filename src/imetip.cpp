// imetip.cpp — 中文输入法光标徽标 (Windows 11 / 微软拼音)
//
// 判据(实测):
//   HKL 低字 == 0x0804  且  (ConversionMode & IME_CMODE_NATIVE)
// 数据来源:
//   GetKeyboardLayout(tid)
//   ImmGetDefaultIMEWnd(fg) + SendMessageTimeout(WM_IME_CONTROL, IMC_GETCONVERSIONMODE)
//
// 功能:
//   - 托盘图标 + 菜单
//   - 开机自启 (启动文件夹快捷方式)
//   - 中文态时在插入符旁显示徽标 (分层窗口, 点击穿透, 不抢焦点)
//   - 前台进入全屏时暂停, 退出全屏后恢复
//
// Build:
//   windres -I src -O coff src\app.rc -o app.rc.o
//   g++ -std=c++17 -O2 -municode -mwindows -static -o imetip.exe src\imetip.cpp app.rc.o ^
//       -limm32 -luser32 -lgdi32 -lshell32 -lshcore -lgdiplus -ladvapi32 -lole32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <objidl.h>   // PROPID (WIN32_LEAN_AND_MEAN 排除了它)
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <oleacc.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <uiautomationclient.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cmath>

using namespace Gdiplus;

// MinGW 的 imm.h 缺这个常量
#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif

#ifndef SMTO_ABORTIFHUNG
#define SMTO_ABORTIFHUNG 0x0002
#endif

// ---------------------------------------------------------------- 常量

static const UINT WM_TRAY = WM_APP + 1;

enum MenuId : UINT {
    IDM_ENABLE = 100,
    IDM_ONLY_EDGE,
    IDM_AUTOSTART,
    IDM_EXIT,
};

static const wchar_t* kOverlayClass = L"ImeTipOverlay";
static const wchar_t* kTrayClass = L"ImeTipTray";

static const int kAppIconRes = 101;      // src\app.rc 里的 dribble.ico
static const wchar_t* kAppIconFile = L"dribble.ico";

// ---------------------------------------------------------------- 全局

static HINSTANCE g_hInst = nullptr;
static HWND      g_overlay = nullptr;
static HWND      g_tray = nullptr;
static HICON     g_icon = nullptr;
static HANDLE    g_mutex = nullptr;
static ULONG_PTR g_gdiToken = 0;

static bool g_enabled = true;    // 总开关
static bool g_onlyEdge = true;   // 仅在 Processes 列表中的进程生效
static bool g_autoStart = true;  // 开机自启
static std::vector<std::wstring> g_processes;  // 允许显示徽标的进程名, 来自 ini
static const wchar_t* kDefaultProcesses = L"msedge.exe,Cursor.exe,Code.exe";
static bool g_preview = false;   // --preview: 强制显示, 用于调试
static bool g_noTray = false;    // --no-tray: 调试用, 跳过托盘
static bool g_debug = false;     // --debug: 状态变化写日志
static bool g_visible = false;
static bool g_chinese = false;   // 最近一次确定的中文态

static int g_w = 0, g_h = 0;
static int g_dpi = 96;
static int g_lastX = INT_MIN, g_lastY = INT_MIN;

static IUIAutomation* g_uia = nullptr;   // 仅用于"焦点是否只读文档"判断
static RECT g_caret = {};                 // 文本插入符, 屏幕坐标
static bool g_haveCaret = false;
static bool g_dbgCh = false, g_dbgCaret = false, g_dbgStale = false;

static const UINT_PTR kTimerFollow = 1;
static const UINT_PTR kTimerPoll = 2;

// MinGW 的 shcore.h 不声明这个, 动态取
typedef HRESULT(WINAPI* PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
static PFN_GetDpiForMonitor g_pGetDpiForMonitor = nullptr;

// ---------------------------------------------------------------- 小工具

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

// 调试日志: 与 exe 同目录的 imetip.exe.log (UTF-8)
static void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, ARRAYSIZE(msg) - 1, fmt, ap);
    va_end(ap);

    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, s.data(), n, nullptr, nullptr);
    s.resize((size_t)n - 1);
    s += "\r\n";

    std::wstring path = ExePath() + L".log";
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f) return;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

static std::wstring ProcessName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        std::wstring full(buf, n);
        size_t p = full.find_last_of(L'\\');
        name = (p == std::wstring::npos) ? full : full.substr(p + 1);
    }
    CloseHandle(h);
    return name;
}

static std::wstring BaseName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

// 配置项可写文件名或完整路径, 省略 .exe 也算匹配
static bool SameProcessName(const std::wstring& proc, const std::wstring& item) {
    std::wstring a = BaseName(proc);
    std::wstring b = BaseName(item);
    if (a.empty() || b.empty()) return false;
    if (_wcsicmp(a.c_str(), b.c_str()) == 0) return true;
    if (b.size() < 4 || _wcsicmp(b.c_str() + b.size() - 4, L".exe") != 0) {
        b += L".exe";
        if (_wcsicmp(a.c_str(), b.c_str()) == 0) return true;
    }
    return false;
}

static bool IsTargetProcess(DWORD pid) {
    std::wstring n = ProcessName(pid);
    for (const auto& item : g_processes) {
        if (SameProcessName(n, item)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 文本插入符

static int FontHeight(HWND hwnd) {
    if (!hwnd) return 0;
    HDC dc = GetDC(hwnd);
    if (!dc) return 0;

    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HFONT created = nullptr;
    if (!font) {
        NONCLIENTMETRICSW ncm = {};
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            created = CreateFontIndirectW(&ncm.lfMessageFont);
            font = created;
        }
    }

    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW tm = {};
    int h = 0;
    if (GetTextMetricsW(dc, &tm)) h = tm.tmHeight;
    if (old) SelectObject(dc, old);
    if (created) DeleteObject(created);
    ReleaseDC(hwnd, dc);
    return h;
}

// 先试 Win32 caret, 失败再试 MSAA OBJID_CARET (VS Code / Edge)
static bool GetCaretRect(RECT& rc) {
    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(0, &gti) && gti.hwndCaret) {
        POINT pt = { gti.rcCaret.left, gti.rcCaret.top };
        if (ClientToScreen(gti.hwndCaret, &pt)) {
            int w = gti.rcCaret.right - gti.rcCaret.left;
            int h = gti.rcCaret.bottom - gti.rcCaret.top;
            if (w < 1) w = 1;
            if (h < 4) {
                int fh = FontHeight(gti.hwndFocus ? gti.hwndFocus : gti.hwndCaret);
                if (fh > h) h = fh;
            }
            if (h < 4) h = MulDiv(16, g_dpi, 96);

            rc.left = pt.x;
            rc.top = pt.y;
            rc.right = pt.x + w;
            rc.bottom = pt.y + h;
            return true;
        }
    }

    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    IAccessible* acc = nullptr;
    if (SUCCEEDED(AccessibleObjectFromWindow(fg, OBJID_CARET, IID_IAccessible,
                                             (void**)&acc)) && acc) {
        VARIANT child;
        VariantInit(&child);
        child.vt = VT_I4;
        child.lVal = CHILDID_SELF;

        long x = 0, y = 0, w = 0, h = 0;
        HRESULT hr = acc->accLocation(&x, &y, &w, &h, child);
        acc->Release();
        if (SUCCEEDED(hr) && (x != 0 || y != 0) && h > 0) {
            // 有选区时 caret 对象覆盖整个选区, 光标在右缘
            rc.left = x + w;
            rc.top = y;
            rc.right = rc.left + 1;
            rc.bottom = y + h;
            return true;
        }
    }
    return false;
}

// 前台焦点是否还在可输入的文字控件上。
// 点击非输入区域(侧栏 / 工具栏 / 网页空白)时, 前台窗口和输入法状态都没变,
// MSAA 的 OBJID_CARET 却会继续返回上一次的位置, 必须靠焦点控件判掉,
// 否则徽标会停在被丢弃的插入符上不消失。
enum class FocusKind {
    Unknown,   // 查询失败, 沿用旧行为
    Editable,  // 焦点在可输入的文字控件上
    ReadOnly,  // 焦点在只读文字控件上(网页正文这类只读 Document)
    Other,     // 焦点在别的控件上(侧栏 / 工具栏 / 按钮 ...)
};

static bool IsRectInside(const RECT& box, const RECT& rc, int tol) {
    return rc.left   >= box.left - tol && rc.left   <= box.right  + tol &&
           rc.right  >= box.left - tol && rc.right  <= box.right  + tol &&
           rc.top    >= box.top  - tol && rc.top    <= box.bottom + tol &&
           rc.bottom >= box.top  - tol && rc.bottom <= box.bottom + tol;
}

// bounds 返回元素范围, 取不到时为空矩形
static FocusKind ClassifyFocusElement(IUIAutomationElement* f, RECT& bounds) {
    CONTROLTYPEID ct = 0;
    if (FAILED(f->get_CurrentControlType(&ct))) return FocusKind::Unknown;

    RECT box = {};
    if (SUCCEEDED(f->get_CurrentBoundingRectangle(&box)) && !IsRectEmpty(&box)) {
        bounds = box;
    }

    bool text = ct == UIA_EditControlTypeId || ct == UIA_DocumentControlTypeId;
    if (!text) return FocusKind::Other;

    // 没暴露 ValuePattern 的 Edit 仍然能输入;
    // Document 只有明确非只读时才算可输入(网页正文是只读 Document)。
    bool editable = ct == UIA_EditControlTypeId;
    IUIAutomationValuePattern* vp = nullptr;
    if (SUCCEEDED(f->GetCurrentPatternAs(UIA_ValuePatternId,
                                        IID_IUIAutomationValuePattern,
                                        (void**)&vp)) && vp) {
        BOOL ro = TRUE;
        if (SUCCEEDED(vp->get_CurrentIsReadOnly(&ro))) editable = ro == FALSE;
        vp->Release();
    }
    return editable ? FocusKind::Editable : FocusKind::ReadOnly;
}

// bounds 返回焦点元素的屏幕范围, 取不到时为空矩形
static FocusKind QueryFocusKind(RECT& bounds) {
    SetRectEmpty(&bounds);
    if (!g_uia) return FocusKind::Unknown;

    IUIAutomationElement* f = nullptr;
    if (FAILED(g_uia->GetFocusedElement(&f)) || !f) return FocusKind::Unknown;

    FocusKind kind = ClassifyFocusElement(f, bounds);
    f->Release();
    return kind;
}

// ---------------------------------------------------------------- IME 查询

enum class ImeState {
    NotTarget,  // 不是目标应用 / 不是微软拼音
    Unknown,    // 查询失败, 保持上一次状态
    Chinese,
    English,
};

static ImeState QueryIme() {
    if (g_preview) return ImeState::Chinese;

    HWND fg = GetForegroundWindow();
    if (!fg) return ImeState::NotTarget;

    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(fg, &pid);
    if (g_onlyEdge && !IsTargetProcess(pid)) return ImeState::NotTarget;

    HKL hkl = GetKeyboardLayout(tid);
    WORD lang = (WORD)((DWORD_PTR)hkl & 0xFFFF);
    if (lang != 0x0804) return ImeState::NotTarget;  // 不是微软拼音

    HWND ime = ImmGetDefaultIMEWnd(fg);
    if (!ime) return ImeState::Unknown;

    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETCONVERSIONMODE, 0,
                             SMTO_ABORTIFHUNG, 500, &result)) {
        return ImeState::Unknown;  // 不要猜成英文
    }
    return (result & IME_CMODE_NATIVE) ? ImeState::Chinese : ImeState::English;
}

// ---------------------------------------------------------------- 徽标绘制

static void RenderBadge(int dpi) {
    if (!g_overlay) return;

    const float sc = (float)dpi / 96.0f;

    FontFamily yahei(L"Microsoft YaHei UI");
    FontFamily segoe(L"Segoe UI");
    FontFamily* family = (yahei.GetLastStatus() == Ok) ? &yahei : &segoe;
    Font font(family, 13.0f * sc, FontStyleBold, UnitPixel);

    StringFormat drawFmt;
    drawFmt.SetAlignment(StringAlignmentCenter);
    drawFmt.SetLineAlignment(StringAlignmentCenter);
    drawFmt.SetFormatFlags(StringFormatFlagsNoWrap);

    // 量 "中" 的实际大小, 让蓝色背景紧贴文字
    RectF bounds;
    {
        Bitmap probe(1, 1, PixelFormat32bppARGB);
        Graphics pg(&probe);
        pg.SetTextRenderingHint(TextRenderingHintAntiAlias);
        StringFormat measureFmt;
        measureFmt.SetAlignment(StringAlignmentNear);
        measureFmt.SetLineAlignment(StringAlignmentNear);
        measureFmt.SetFormatFlags(StringFormatFlagsNoWrap);
        pg.MeasureString(L"中", -1, &font, PointF(0.0f, 0.0f), &measureFmt, &bounds);
    }

    const int padX = (int)std::lround(2.0f * sc);
    const int padY = (int)std::lround(1.0f * sc);
    int w = (int)std::ceil(bounds.Width) + padX * 2;
    int h = (int)std::ceil(bounds.Height) + padY * 2;
    if (w < 8) w = 8;
    if (h < 8) h = 8;

    HDC screen = GetDC(nullptr);
    if (!screen) return;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        ReleaseDC(nullptr, screen);
        return;
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, hbm);

    {
        Bitmap bmp(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeNone);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));

        // 纯矩形背景, 无圆角
        SolidBrush bg(Color(238, 21, 101, 192));  // #1565C0
        g.FillRectangle(&bg, 0, 0, w, h);

        SolidBrush fg(Color(255, 255, 255, 255));
        RectF rc(0.0f, 0.0f, (REAL)w, (REAL)h);
        g.DrawString(L"中", -1, &font, rc, &drawFmt, &fg);
    }

    // 直通 alpha -> 预乘 alpha (UpdateLayeredWindow 要求)
    BYTE* p = (BYTE*)bits;
    for (int i = 0; i < w * h; ++i, p += 4) {
        BYTE a = p[3];
        p[0] = (BYTE)(p[0] * a / 255);
        p[1] = (BYTE)(p[1] * a / 255);
        p[2] = (BYTE)(p[2] * a / 255);
    }

    RECT wr = {};
    GetWindowRect(g_overlay, &wr);
    POINT dst = {wr.left, wr.top};
    POINT src = {0, 0};
    SIZE sz = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_overlay, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(hbm);
    ReleaseDC(nullptr, screen);

    g_w = w;
    g_h = h;
    g_lastX = INT_MIN;
    g_lastY = INT_MIN;
}

static void PlaceOverlay() {
    if (!g_overlay) return;
    if (!g_w || !g_h) RenderBadge(g_dpi);

    const bool caretMode = !g_preview;
    POINT pt = {};
    if (caretMode) {
        if (!g_haveCaret) return;
        pt.x = g_caret.left;
        pt.y = g_caret.bottom;
    } else {
        GetCursorPos(&pt);
    }

    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon) mon = MonitorFromWindow(g_overlay, MONITOR_DEFAULTTONEAREST);

    UINT dx = 96, dy = 96;
    if (g_pGetDpiForMonitor && SUCCEEDED(g_pGetDpiForMonitor(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) &&
        (int)dx != g_dpi && dx > 0) {
        g_dpi = (int)dx;
        RenderBadge(g_dpi);
    }

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    int x = 0, y = 0;
    if (caretMode) {
        // 徽标左侧与插入符左边对齐, 放在插入符下方
        const int off = MulDiv(2, g_dpi, 96);
        x = g_caret.left;
        y = g_caret.bottom + off;
        if (y + g_h > mi.rcMonitor.bottom) y = g_caret.top - g_h - off;
    } else {
        const int off = MulDiv(18, g_dpi, 96);
        x = pt.x + off;
        y = pt.y + off;
        if (x + g_w > mi.rcMonitor.right)  x = pt.x - off - g_w;
        if (y + g_h > mi.rcMonitor.bottom) y = pt.y - off - g_h;
    }

    if (x < mi.rcMonitor.left) x = mi.rcMonitor.left;
    if (y < mi.rcMonitor.top)  y = mi.rcMonitor.top;
    if (x + g_w > mi.rcMonitor.right)  x = mi.rcMonitor.right - g_w;
    if (y + g_h > mi.rcMonitor.bottom) y = mi.rcMonitor.bottom - g_h;

    if (x != g_lastX || y != g_lastY) {
        SetWindowPos(g_overlay, HWND_TOPMOST, x, y, g_w, g_h,
                     SWP_NOACTIVATE | (g_visible ? SWP_SHOWWINDOW : 0));
        g_lastX = x;
        g_lastY = y;
    }
}

static void ShowOverlay() {
    if (!g_overlay) return;
    if (!g_w || !g_h) RenderBadge(g_dpi);
    g_visible = true;
    PlaceOverlay();
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
    SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void HideOverlay() {
    if (!g_overlay) return;
    g_visible = false;
    g_lastX = INT_MIN;
    g_lastY = INT_MIN;
    ShowWindow(g_overlay, SW_HIDE);
}

static void ApplyState() {
    if (g_preview) {
        const bool show = g_enabled;
        if (g_debug && show != g_visible) Log(L"preview show=%d", show ? 1 : 0);
        if (show && !g_visible) ShowOverlay();
        else if (!show && g_visible) HideOverlay();
        else if (show && g_visible) PlaceOverlay();
        return;
    }

    ImeState s = QueryIme();
    if (s == ImeState::Chinese) {
        g_chinese = true;
    } else if (s == ImeState::English || s == ImeState::NotTarget) {
        g_chinese = false;
    }
    // Unknown: 保持 g_chinese 不变

    g_haveCaret = false;
    bool stale = false;   // 找到的插入符已失效: 焦点离开了编辑框
    RECT rc = {};
    if (g_chinese) {
        if (GetCaretRect(rc)) {
            RECT box = {};
            FocusKind focus = QueryFocusKind(box);
            const int tol = MulDiv(4, g_dpi, 96);
            if (focus == FocusKind::ReadOnly) {
                // 只读文档里没有可输入的光标
                stale = true;
            } else if (focus == FocusKind::Other && !IsRectEmpty(&box) &&
                       !IsRectInside(box, rc, tol)) {
                // 点击了按钮 / 侧栏 / 工具栏: 焦点已不在编辑框上,
                // 但 MSAA 仍返回旧插入符, 这时要收起徽标, 不能停在最后位置
                stale = true;
            } else {
                g_caret = rc;
                g_haveCaret = true;
            }
        }
    }

    if (g_debug && (g_chinese != g_dbgCh || g_haveCaret != g_dbgCaret || stale != g_dbgStale)) {
        Log(L"probe chinese=%d caret=%d stale=%d rect=%ld,%ld,%ld,%ld",
            g_chinese ? 1 : 0, g_haveCaret ? 1 : 0, stale ? 1 : 0,
            rc.left, rc.top, rc.right, rc.bottom);
        g_dbgCh = g_chinese;
        g_dbgCaret = g_haveCaret;
        g_dbgStale = stale;
    }

    const bool show = g_enabled && g_chinese && g_haveCaret;
    if (g_debug && show != g_visible) {
        Log(L"state=%d chinese=%d caret=%d rect=%ld,%ld,%ld,%ld show=%d",
            (int)s, g_chinese ? 1 : 0, g_haveCaret ? 1 : 0,
            g_caret.left, g_caret.top, g_caret.right, g_caret.bottom,
            show ? 1 : 0);
    }
    if (show && !g_visible) ShowOverlay();
    else if (!show && g_visible) HideOverlay();
    else if (show && g_visible) PlaceOverlay();
}

// ---------------------------------------------------------------- 窗口过程

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------- 设置 / 自启

static std::wstring IniPath() {
    std::wstring p = ExePath();
    size_t dot = p.find_last_of(L'.');
    size_t slash = p.find_last_of(L'\\');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        p.resize(dot);
    }
    return p + L".ini";
}

static bool IniExists() {
    return GetFileAttributesW(IniPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

static int IniGet(const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(L"ImeTip", key, (UINT)def, IniPath().c_str());
}

static void IniSet(const wchar_t* key, int value) {
    wchar_t buf[16] = {};
    swprintf(buf, 16, L"%d", value);
    WritePrivateProfileStringW(L"ImeTip", key, buf, IniPath().c_str());
}

static std::wstring IniGetStr(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[4096] = {};
    GetPrivateProfileStringW(L"ImeTip", key, def, buf, ARRAYSIZE(buf), IniPath().c_str());
    return buf;
}

static void IniSetStr(const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(L"ImeTip", key, value.c_str(), IniPath().c_str());
}

static bool IsListSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

// 逗号 / 分号 / 竖线分隔, 两边空白忽略
static std::vector<std::wstring> ParseProcessList(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&]() {
        size_t a = 0, b = cur.size();
        while (a < b && IsListSpace(cur[a])) ++a;
        while (b > a && IsListSpace(cur[b - 1])) --b;
        if (b > a) out.emplace_back(cur, a, b - a);
        cur.clear();
    };
    for (wchar_t c : s) {
        if (c == L',' || c == L';' || c == L'|') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

static std::wstring JoinProcessList(const std::vector<std::wstring>& names) {
    std::wstring s;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) s += L',';
        s += names[i];
    }
    return s;
}

static void SaveSettings() {
    IniSet(L"Enabled", g_enabled ? 1 : 0);
    IniSet(L"OnlyEdge", g_onlyEdge ? 1 : 0);
    IniSet(L"AutoStart", g_autoStart ? 1 : 0);
    IniSetStr(L"Processes", JoinProcessList(g_processes));
    Log(L"ini saved enabled=%d onlyEdge=%d autoStart=%d processes=%s path=%s",
        g_enabled ? 1 : 0, g_onlyEdge ? 1 : 0, g_autoStart ? 1 : 0,
        JoinProcessList(g_processes).c_str(), IniPath().c_str());
}

static void LoadProcessList() {
    // 缺键时默认串不会出现在用户配置里, 用来区分"没写"和"写了空列表"
    const wchar_t* kMissing = L"\x1";
    std::wstring s = IniGetStr(L"Processes", kMissing);
    if (s == kMissing) {
        g_processes = ParseProcessList(kDefaultProcesses);
        IniSetStr(L"Processes", kDefaultProcesses);
        Log(L"ini processes defaulted: %s", kDefaultProcesses);
        return;
    }
    g_processes = ParseProcessList(s);
}

static void LoadSettings() {
    if (!IniExists()) {
        g_enabled = true;
        g_onlyEdge = true;
        g_autoStart = true;
        g_processes = ParseProcessList(kDefaultProcesses);
        SaveSettings();
        Log(L"ini created: %s", IniPath().c_str());
        return;
    }
    g_enabled   = IniGet(L"Enabled", 1) != 0;
    g_onlyEdge  = IniGet(L"OnlyEdge", 1) != 0;
    g_autoStart = IniGet(L"AutoStart", 1) != 0;
    LoadProcessList();
    Log(L"ini loaded enabled=%d onlyEdge=%d autoStart=%d processes=%s path=%s",
        g_enabled ? 1 : 0, g_onlyEdge ? 1 : 0, g_autoStart ? 1 : 0,
        JoinProcessList(g_processes).c_str(), IniPath().c_str());
}

// ---------------------------------------------------------------- 开机自启

static std::wstring StartupLnkPath() {
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr,
                                SHGFP_TYPE_CURRENT, path))) {
        return L"";
    }
    std::wstring p = path;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    return p + L"ImeTip.lnk";
}

static bool WriteStartupShortcut() {
    std::wstring lnk = StartupLnkPath();
    if (lnk.empty()) return false;

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)) || !link) {
        return false;
    }

    std::wstring exe = ExePath();
    std::wstring dir = exe;
    size_t k = dir.find_last_of(L'\\');
    if (k != std::wstring::npos) dir.resize(k);

    link->SetPath(exe.c_str());
    link->SetArguments(L"--autostart");
    link->SetWorkingDirectory(dir.c_str());
    link->SetDescription(L"ImeTip - 中文输入法插入符提示");

    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

static void RemoveStartupShortcut() {
    std::wstring lnk = StartupLnkPath();
    if (!lnk.empty()) DeleteFileW(lnk.c_str());
}

static void ApplyAutoStart(bool on) {
    if (on) {
        bool ok = WriteStartupShortcut();
        Log(L"startup shortcut on ok=%d path=%s", ok ? 1 : 0,
            StartupLnkPath().c_str());
    } else {
        RemoveStartupShortcut();
        Log(L"startup shortcut removed");
    }
}

// ---------------------------------------------------------------- 托盘

static void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : 0), IDM_ENABLE, L"显示状态标");
    AppendMenuW(menu, MF_STRING | (g_onlyEdge ? MF_CHECKED : 0), IDM_ONLY_EDGE, L"仅指定进程");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_autoStart ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_tray);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, g_tray, nullptr);
    PostMessageW(g_tray, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == kTimerFollow) {
            if (g_visible) PlaceOverlay();
        } else if (wp == kTimerPoll) {
            ApplyState();
        }
        return 0;

    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            ShowMenu();
        } else if (lp == WM_LBUTTONDBLCLK) {
            g_enabled = !g_enabled;
            SaveSettings();
            ApplyState();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_ENABLE:
            g_enabled = !g_enabled;
            SaveSettings();
            ApplyState();
            break;
        case IDM_ONLY_EDGE:
            g_onlyEdge = !g_onlyEdge;
            SaveSettings();
            ApplyState();
            break;
        case IDM_AUTOSTART:
            g_autoStart = !g_autoStart;
            SaveSettings();
            ApplyAutoStart(g_autoStart);
            break;
        case IDM_EXIT:
            DestroyWindow(g_tray);
            break;
        }
        return 0;

    case WM_DESTROY: {
        KillTimer(hwnd, kTimerFollow);
        KillTimer(hwnd, kTimerPoll);
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (g_icon) {
            DestroyIcon(g_icon);
            g_icon = nullptr;
        }
        if (g_overlay) {
            DestroyWindow(g_overlay);
            g_overlay = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HICON MakeIcon(int sz) {
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz;
    bmi.bmiHeader.biHeight = -sz;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) {
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    {
        Bitmap bmp(sz, sz, sz * 4, PixelFormat32bppARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeNone);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));

        SolidBrush bg(Color(255, 21, 101, 192));
        g.FillRectangle(&bg, 0, 0, sz, sz);

        FontFamily yahei(L"Microsoft YaHei");
        FontFamily* family = &yahei;
        Font font(family, sz * 0.46f, FontStyleRegular, UnitPixel);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter );
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush fg(Color(255, 255, 255, 255));
        RectF rc(0.0f, 0.0f, (REAL)sz, (REAL)sz);
        g.DrawString(L"中", -1, &font, rc, &sf, &fg);
    }

    std::vector<BYTE> zero((size_t)((sz * sz + 7) / 8), 0);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, zero.data());

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    if (mask) DeleteObject(mask);
    DeleteObject(color);
    ReleaseDC(nullptr, screen);
    return icon;
}

// 程序图标: 先取编进 exe 的 dribble.ico 资源, 再取 exe 同目录的 dribble.ico,
// 都没有才退回代码绘制的 "中" 图标
static HICON LoadAppIcon(int sz) {
    HICON icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(kAppIconRes),
                                   IMAGE_ICON, sz, sz, 0);
    if (icon) return icon;

    std::wstring path = ExePath();
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
        path += kAppIconFile;
        icon = (HICON)LoadImageW(nullptr, path.c_str(), IMAGE_ICON, sz, sz,
                                 LR_LOADFROMFILE);
        if (icon) return icon;
    }

    return MakeIcon(sz);
}

static bool CreateTray() {
    g_icon = LoadAppIcon(32);
    if (!g_icon) g_icon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = g_hInst;
    wc.hIcon = g_icon;
    wc.hIconSm = g_icon;
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    g_tray = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, L"ImeTip",
                             WS_OVERLAPPED, 0, 0, 0, 0,
                             nullptr, nullptr, g_hInst, nullptr);
    if (!g_tray) return false;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_tray;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    wcsncpy(nid.szTip, L"ImeTip - 中文输入法可视化", ARRAYSIZE(nid.szTip) - 1);

    BOOL added = FALSE;
    DWORD trayErr = 0;
    if (!g_noTray) {
        added = Shell_NotifyIconW(NIM_ADD, &nid);
        trayErr = GetLastError();
        if (!added) {
            // 自定义图标可能有问题, 退回系统图标再试一次
            nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            added = Shell_NotifyIconW(NIM_ADD, &nid);
            Log(L"tray retry add=%d err=%lu", added, GetLastError());
        }
    }
    Log(L"Shell_NotifyIcon add=%d err=%lu tray=%p icon=%p noTray=%d cbSize=%zu",
        added, trayErr, g_tray, g_icon, g_noTray ? 1 : 0, sizeof(NOTIFYICONDATAW));

    SetTimer(g_tray, kTimerFollow, 16, nullptr);
    SetTimer(g_tray, kTimerPoll, 80, nullptr);
    return true;
}

static bool CreateOverlay() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kOverlayClass, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
    if (!g_overlay) return false;

    RenderBadge(g_dpi);
    return true;
}

// ---------------------------------------------------------------- 入口

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    g_hInst = hInst;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--preview") == 0) g_preview = true;
            if (wcscmp(argv[i], L"--no-tray") == 0) g_noTray = true;
            if (wcscmp(argv[i], L"--debug") == 0) g_debug = true;
        }
        LocalFree(argv);
    }

    // 单实例
    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\ImeTip.SingleInstance");
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        g_pGetDpiForMonitor =
            (PFN_GetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor");
    }

    Log(L"=== start preview=%d", g_preview ? 1 : 0);
    {
        wchar_t wsn[256] = {};
        wchar_t dsn[256] = {};
        DWORD n = 0;
        GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, wsn, sizeof(wsn), &n);
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, dsn, sizeof(dsn), &n);
        Log(L"winsta=%s desktop=%s", wsn, dsn);
    }

    HRESULT ole = OleInitialize(nullptr);
    Log(L"OleInitialize hr=0x%08X", (unsigned)ole);
    if (SUCCEEDED(ole)) {
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IUIAutomation, (void**)&g_uia))) {
            g_uia = nullptr;
        }
        Log(L"uia=%p", g_uia);
    }

    GdiplusStartupInput gsi;
    HRESULT gp = GdiplusStartup(&g_gdiToken, &gsi, nullptr);
    Log(L"GdiplusStartup hr=0x%08X", (unsigned)gp);
    if (gp != Ok) {
        if (g_uia) { g_uia->Release(); g_uia = nullptr; }
        if (SUCCEEDED(ole)) OleUninitialize();
        return 1;
    }

    LoadSettings();
    if (!g_preview) {
        ApplyAutoStart(g_autoStart);  // 同步启动文件夹快捷方式
    }

    if (!CreateOverlay() || !CreateTray()) {
        Log(L"create failed overlay=%p tray=%p", g_overlay, g_tray);
        if (g_overlay) DestroyWindow(g_overlay);
        if (g_uia) { g_uia->Release(); g_uia = nullptr; }
        if (SUCCEEDED(ole)) OleUninitialize();
        GdiplusShutdown(g_gdiToken);
        if (g_mutex) CloseHandle(g_mutex);
        return 1;
    }

    g_dpi = 96;
    ApplyState();
    Log(L"running overlay=%p tray=%p", g_overlay, g_tray);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_uia) {
        g_uia->Release();
        g_uia = nullptr;
    }
    if (SUCCEEDED(ole)) OleUninitialize();

    GdiplusShutdown(g_gdiToken);
    if (g_mutex) {
        CloseHandle(g_mutex);
 