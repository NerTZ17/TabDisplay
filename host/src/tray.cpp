#include <windows.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <io.h>
#include <share.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "engine.h"

using Microsoft::WRL::ComPtr;
using namespace td;

namespace {

constexpr wchar_t kWindowClass[] = L"TabDisplayTrayPanel";
constexpr wchar_t kSettingsKey[] = L"Software\\NerTZ\\TabDisplay";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"TabDisplay";
constexpr wchar_t kShowEvent[] = L"Local\\TabDisplayShow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowMessage = WM_APP + 2;
constexpr UINT kStoppedMessage = WM_APP + 3;
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kAnimTimer = 2;
constexpr UINT kTrayId = 1;
constexpr UINT kMenuOpen = 100;
constexpr UINT kMenuExit = 101;

constexpr float kWidth = 320.0f;
constexpr float kPad = 16.0f;

struct Palette {
    D2D1_COLOR_F bg;
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F tertiary;
    D2D1_COLOR_F divider;
    D2D1_COLOR_F controlStroke;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F onAccent;
    D2D1_COLOR_F good;
    D2D1_COLOR_F wait;
    D2D1_COLOR_F bad;
    D2D1_COLOR_F hover;
    COLORREF border;
};

D2D1_COLOR_F rgb(uint32_t v, float a = 1.0f) {
    return D2D1::ColorF(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, a);
}

bool readDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD& out) {
    DWORD size = sizeof out;
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}

void writeDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof value);
}

bool systemLight() {
    DWORD v = 0;
    return readDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", v) && v != 0;
}

bool animationsEnabled() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

Palette makePalette(bool light) {
    Palette p{};
    uint8_t accent[32]{};
    DWORD size = sizeof accent;
    bool haveAccent = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                                   RRF_RT_REG_BINARY, nullptr, accent, &size) == ERROR_SUCCESS && size >= 32;
    auto paletteColor = [&](int index, uint32_t fallback) -> D2D1_COLOR_F {
        if (!haveAccent) {
            return rgb(fallback);
        }
        const uint8_t* c = accent + index * 4;
        return D2D1::ColorF(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f);
    };
    if (light) {
        p.bg = rgb(0xF9F9F9);
        p.text = rgb(0x1B1B1B);
        p.secondary = rgb(0x5C5C5C);
        p.tertiary = rgb(0x8A8A8A);
        p.divider = rgb(0xE5E5E5);
        p.controlStroke = rgb(0x878787);
        p.accent = paletteColor(4, 0x005FB8);
        p.onAccent = rgb(0xFFFFFF);
        p.good = rgb(0x0F7B0F);
        p.wait = rgb(0x9D5D00);
        p.bad = rgb(0xC42B1C);
        p.hover = rgb(0x000000, 0.04f);
        p.border = RGB(0xD9, 0xD9, 0xD9);
    } else {
        p.bg = rgb(0x2C2C2C);
        p.text = rgb(0xFFFFFF);
        p.secondary = rgb(0xC8C8C8);
        p.tertiary = rgb(0x9E9E9E);
        p.divider = rgb(0x3D3D3D);
        p.controlStroke = rgb(0x9E9E9E);
        p.accent = paletteColor(1, 0x60CDFF);
        p.onAccent = rgb(0x000000);
        p.good = rgb(0x6CCB5F);
        p.wait = rgb(0xFCE100);
        p.bad = rgb(0xFF99A4);
        p.hover = rgb(0xFFFFFF, 0.06f);
        p.border = RGB(0x45, 0x45, 0x45);
    }
    return p;
}

enum class Focus { None, Power, Info, Autostart };

struct App {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    UINT taskbarCreated = 0;
    NOTIFYICONDATAW nid{};
    HICON icon = nullptr;
    std::unique_ptr<Engine> engine;
    std::thread stopper;
    std::atomic<bool> stopping{false};
    bool exiting = false;
    bool enabled = true;
    bool autostart = true;
    bool showInfo = false;
    bool light = false;
    Palette pal{};
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<IDWriteTextFormat> titleFormat;
    ComPtr<IDWriteTextFormat> bodyFormat;
    ComPtr<IDWriteTextFormat> captionFormat;
    ComPtr<IDWriteTextFormat> iconFormat;
    EngineStatus status;
    std::wstring lastTip;
    int lastIconKind = -1;
    Focus focus = Focus::None;
    Focus hover = Focus::None;
    bool keyboardFocus = false;
    float powerKnob = 1.0f;
    float autoKnob = 1.0f;
    D2D1_RECT_F powerRect{};
    D2D1_RECT_F infoRect{};
    D2D1_RECT_F autoRowRect{};
    D2D1_RECT_F autoRect{};
    float height = 0;
};

App g;

std::wstring exePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

bool autostartRegistered() {
    wchar_t value[1024];
    DWORD size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

void setAutostart(bool on) {
    if (on) {
        std::wstring cmd = L"\"" + exePath() + L"\" --background";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(), static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
    g.autostart = on;
}

void loadSettings() {
    DWORD initialized = 0;
    if (!readDword(HKEY_CURRENT_USER, kSettingsKey, L"Initialized", initialized) || !initialized) {
        writeDword(L"Initialized", 1);
        writeDword(L"Enabled", 1);
        setAutostart(true);
    }
    DWORD enabled = 1;
    readDword(HKEY_CURRENT_USER, kSettingsKey, L"Enabled", enabled);
    g.enabled = enabled != 0;
    g.autostart = autostartRegistered();
    g.powerKnob = g.enabled ? 1.0f : 0.0f;
    g.autoKnob = g.autostart ? 1.0f : 0.0f;
}

void openLog() {
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    std::filesystem::path dir = std::filesystem::path(local) / L"TabDisplay";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::wstring log = (dir / L"tabdisplay.log").wstring();
    FILE* f = nullptr;
    _wfreopen_s(&f, L"NUL", L"w", stdout);
    _wfreopen_s(&f, L"NUL", L"w", stderr);
    FILE* shared = _wfsopen(log.c_str(), L"w", _SH_DENYNO);
    if (shared) {
        _dup2(_fileno(shared), _fileno(stdout));
        _dup2(_fileno(shared), _fileno(stderr));
    }
}

enum IconKind { IconOff = 0, IconWait = 1, IconGood = 2, IconBad = 3 };

int iconKindFor(const EngineStatus& s, bool enabled) {
    if (!enabled) {
        return IconOff;
    }
    switch (s.state) {
        case EngineState::Streaming:
            return IconGood;
        case EngineState::TabletProblem:
        case EngineState::Error:
            return IconBad;
        case EngineState::Off:
        case EngineState::Stopping:
            return IconOff;
        default:
            return IconWait;
    }
}

HICON makeIcon(int kind) {
    int size = GetSystemMetrics(SM_CXSMICON);
    UINT dpi = GetDpiForSystem();
    size = MulDiv(16, static_cast<int>(dpi), 96);
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(g.wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
        return nullptr;
    }
    ComPtr<ID2D1RenderTarget> rt;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f * size / 16.0f,
        96.0f * size / 16.0f);
    if (FAILED(g.d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) {
        return nullptr;
    }
    bool lightBar = systemLight();
    D2D1_COLOR_F ink = lightBar ? rgb(0x1B1B1B) : rgb(0xFFFFFF);
    if (kind == IconOff) {
        ink.a = 0.55f;
    }
    Palette barPal = makePalette(lightBar);
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(ink, &brush);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1.5f, 3.5f, 14.5f, 12.5f), 1.6f, 1.6f), brush.Get(), 1.25f);
    rt->FillRectangle(D2D1::RectF(6.0f, 13.5f, 10.0f, 14.6f), brush.Get());
    if (kind != IconOff) {
        D2D1_COLOR_F dot = kind == IconGood ? barPal.good : kind == IconBad ? barPal.bad : barPal.wait;
        ComPtr<ID2D1SolidColorBrush> dotBrush;
        rt->CreateSolidColorBrush(dot, &dotBrush);
        ComPtr<ID2D1SolidColorBrush> ring;
        rt->CreateSolidColorBrush(lightBar ? rgb(0xF3F3F3) : rgb(0x202020), &ring);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(12.2f, 11.8f), 4.0f, 4.0f), ring.Get());
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(12.2f, 11.8f), 3.0f, 3.0f), dotBrush.Get());
    }
    if (FAILED(rt->EndDraw())) {
        return nullptr;
    }
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) {
        return nullptr;
    }
    bitmap->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE*>(bits));
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

std::wstring statusTitle(const EngineStatus& s) {
    if (g.stopping) {
        return L"Turning off…";
    }
    if (!g.enabled) {
        return L"Off";
    }
    switch (s.state) {
        case EngineState::Streaming:
            return L"Connected";
        case EngineState::NoTablet:
            return L"Waiting for tablet";
        case EngineState::OpeningApp:
            return s.detail.empty() ? L"Opening the tablet app" : s.detail;
        case EngineState::TabletProblem:
        case EngineState::Error:
            return s.detail;
        case EngineState::Stopping:
            return L"Turning off…";
        default:
            return L"Starting…";
    }
}

std::wstring streamLine(const EngineStatus& s) {
    if (!g.enabled || s.state != EngineState::Streaming) {
        return {};
    }
    wchar_t buf[96];
    swprintf_s(buf, L"%u × %u · %u fps", s.width, s.height, s.fps);
    return buf;
}

std::wstring infoLine(const EngineStatus& s) {
    wchar_t buf[160];
    std::wstring encoder(s.encoder.begin(), s.encoder.end());
    swprintf_s(buf, L"Latency %.0f ms · %.0f fps · %.0f Mbps · %s %s", s.rttMs, s.ackFps, s.mbps, s.hevc ? L"HEVC" : L"H.264", encoder.c_str());
    return buf;
}

void updateTray(bool force) {
    int kind = iconKindFor(g.status, g.enabled && !g.stopping);
    std::wstring tip = L"TabDisplay · " + statusTitle(g.status);
    if (!force && kind == g.lastIconKind && tip == g.lastTip) {
        return;
    }
    if (kind != g.lastIconKind || force) {
        HICON next = makeIcon(kind);
        if (next) {
            if (g.icon) {
                DestroyIcon(g.icon);
            }
            g.icon = next;
        }
        g.lastIconKind = kind;
    }
    g.lastTip = tip;
    g.nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g.nid.hIcon = g.icon;
    wcsncpy_s(g.nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

void addTray() {
    g.nid = {};
    g.nid.cbSize = sizeof g.nid;
    g.nid.hWnd = g.hwnd;
    g.nid.uID = kTrayId;
    g.nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    g.nid.uCallbackMessage = kTrayMessage;
    g.lastIconKind = -1;
    g.icon = makeIcon(IconOff);
    g.nid.hIcon = g.icon;
    wcsncpy_s(g.nid.szTip, L"TabDisplay", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g.nid);
    g.nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
    updateTray(true);
}

Options engineOptions() {
    Options o;
    return o;
}

void startEngine() {
    if (g.engine || g.stopping) {
        return;
    }
    g.engine = std::make_unique<Engine>(engineOptions());
    g.engine->start();
}

void stopEngineAsync() {
    if (!g.engine || g.stopping) {
        return;
    }
    g.stopping = true;
    if (g.stopper.joinable()) {
        g.stopper.join();
    }
    Engine* engine = g.engine.get();
    g.stopper = std::thread([engine] {
        engine->stop();
        PostMessageW(g.hwnd, kStoppedMessage, 0, 0);
    });
}

void createFormats() {
    const wchar_t* text = L"Segoe UI Variable Text";
    const wchar_t* display = L"Segoe UI Variable Display";
    ComPtr<IDWriteFontCollection> fonts;
    g.dwrite->GetSystemFontCollection(&fonts);
    UINT32 index = 0;
    BOOL exists = FALSE;
    fonts->FindFamilyName(text, &index, &exists);
    if (!exists) {
        text = L"Segoe UI";
        display = L"Segoe UI";
    }
    const wchar_t* iconFont = L"Segoe Fluent Icons";
    fonts->FindFamilyName(iconFont, &index, &exists);
    if (!exists) {
        iconFont = L"Segoe MDL2 Assets";
    }
    g.dwrite->CreateTextFormat(display, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us",
                               &g.titleFormat);
    g.dwrite->CreateTextFormat(text, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us",
                               &g.bodyFormat);
    g.dwrite->CreateTextFormat(text, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us",
                               &g.captionFormat);
    g.dwrite->CreateTextFormat(iconFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us",
                               &g.iconFormat);
    for (IDWriteTextFormat* f : {g.titleFormat.Get(), g.bodyFormat.Get(), g.captionFormat.Get(), g.iconFormat.Get()}) {
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    g.iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

float layoutHeight() {
    float h = kPad + 32.0f + 6.0f + 22.0f;
    if (!streamLine(g.status).empty()) {
        h += 20.0f;
    }
    if (g.showInfo && g.enabled && g.status.state == EngineState::Streaming) {
        h += 20.0f;
    }
    h += 14.0f + 1.0f + 6.0f + 40.0f + 8.0f;
    return h;
}

void applyTheme() {
    g.light = systemLight();
    g.pal = makePalette(g.light);
    if (g.hwnd) {
        BOOL dark = g.light ? FALSE : TRUE;
        DwmSetWindowAttribute(g.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
        COLORREF border = g.pal.border;
        DwmSetWindowAttribute(g.hwnd, DWMWA_BORDER_COLOR, &border, sizeof border);
    }
}

float dpiScale() { return static_cast<float>(GetDpiForWindow(g.hwnd)) / 96.0f; }

void positionPanel() {
    float scale = dpiScale();
    int w = static_cast<int>(std::lround(kWidth * scale));
    g.height = layoutHeight();
    int h = static_cast<int>(std::lround(g.height * scale));
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof id;
    id.hWnd = g.hwnd;
    id.uID = kTrayId;
    RECT iconRect{};
    POINT anchor{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &iconRect))) {
        anchor = {(iconRect.left + iconRect.right) / 2, (iconRect.top + iconRect.bottom) / 2};
    } else {
        GetCursorPos(&anchor);
    }
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT work = mi.rcWork;
    RECT full = mi.rcMonitor;
    int margin = static_cast<int>(std::lround(12.0f * scale));
    int x = anchor.x - w / 2;
    int y = work.bottom - h - margin;
    if (work.top > full.top) {
        y = work.top + margin;
    }
    if (work.left > full.left) {
        x = work.left + margin;
        y = anchor.y - h / 2;
    } else if (work.right < full.right) {
        x = work.right - w - margin;
        y = anchor.y - h / 2;
    }
    x = std::clamp(x, static_cast<int>(work.left) + margin, static_cast<int>(work.right) - w - margin);
    y = std::clamp(y, static_cast<int>(work.top) + margin, static_cast<int>(work.bottom) - h - margin);
    SetWindowPos(g.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    if (g.target) {
        g.target->Resize(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)));
    }
}

void showPanel() {
    g.status = g.engine ? g.engine->status() : EngineStatus{};
    applyTheme();
    positionPanel();
    g.focus = Focus::Power;
    g.keyboardFocus = false;
    ShowWindow(g.hwnd, SW_SHOW);
    SetForegroundWindow(g.hwnd);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void hidePanel() {
    ShowWindow(g.hwnd, SW_HIDE);
    g.showInfo = false;
}

bool ensureTarget() {
    if (g.target) {
        return true;
    }
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    float dpi = static_cast<float>(GetDpiForWindow(g.hwnd));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    return SUCCEEDED(g.d2d->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(g.hwnd, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom))), &g.target));
}

void drawText(const std::wstring& s, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    g.target->CreateSolidColorBrush(color, &brush);
    g.target->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), format, rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void drawFocus(const D2D1_RECT_F& r, float radius) {
    ComPtr<ID2D1SolidColorBrush> brush;
    g.target->CreateSolidColorBrush(g.pal.text, &brush);
    D2D1_RECT_F outer = D2D1::RectF(r.left - 3.0f, r.top - 3.0f, r.right + 3.0f, r.bottom + 3.0f);
    g.target->DrawRoundedRectangle(D2D1::RoundedRect(outer, radius + 3.0f, radius + 3.0f), brush.Get(), 2.0f);
}

void drawToggle(const D2D1_RECT_F& r, float knob, bool hovered, bool focused) {
    float radius = (r.bottom - r.top) / 2.0f;
    D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(r, radius, radius);
    ComPtr<ID2D1SolidColorBrush> brush;
    D2D1_COLOR_F off = g.pal.controlStroke;
    D2D1_COLOR_F accent = g.pal.accent;
    D2D1_COLOR_F fill = D2D1::ColorF(accent.r, accent.g, accent.b, knob);
    g.target->CreateSolidColorBrush(fill, &brush);
    g.target->FillRoundedRectangle(pill, brush.Get());
    if (knob < 1.0f) {
        brush->SetColor(D2D1::ColorF(off.r, off.g, off.b, 1.0f - knob));
        D2D1_RECT_F inner = D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f);
        g.target->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius - 0.5f, radius - 0.5f), brush.Get(), 1.0f);
    }
    float size = (hovered ? 14.0f : 12.0f) + 2.0f * knob;
    size = std::min(size, (r.bottom - r.top) - 4.0f);
    float travel = (r.right - r.left) - (r.bottom - r.top);
    float cx = r.left + radius + travel * knob;
    float cy = (r.top + r.bottom) / 2.0f;
    D2D1_COLOR_F knobColor = knob >= 0.5f ? g.pal.onAccent : g.pal.secondary;
    brush->SetColor(knobColor);
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), size / 2.0f - 2.0f, size / 2.0f - 2.0f), brush.Get());
    if (focused) {
        drawFocus(r, radius);
    }
}

void paint() {
    if (!ensureTarget()) {
        return;
    }
    const Palette& p = g.pal;
    g.target->BeginDraw();
    g.target->Clear(p.bg);
    float x = kPad;
    float right = kWidth - kPad;
    float y = kPad;

    drawText(L"TabDisplay", g.titleFormat.Get(), D2D1::RectF(x, y, right - 60.0f, y + 32.0f), p.text);
    g.powerRect = D2D1::RectF(right - 40.0f, y + 6.0f, right, y + 26.0f);
    drawToggle(g.powerRect, g.powerKnob, g.hover == Focus::Power, g.keyboardFocus && g.focus == Focus::Power);
    y += 32.0f + 6.0f;

    EngineState st = g.stopping ? EngineState::Stopping : (g.enabled ? g.status.state : EngineState::Off);
    D2D1_COLOR_F dot = p.tertiary;
    if (st == EngineState::Streaming) {
        dot = p.good;
    } else if (st == EngineState::TabletProblem || st == EngineState::Error) {
        dot = p.bad;
    } else if (st == EngineState::NoTablet || st == EngineState::OpeningApp || st == EngineState::Stopping) {
        dot = p.wait;
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    g.target->CreateSolidColorBrush(dot, &brush);
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 4.0f, y + 11.0f), 4.0f, 4.0f), brush.Get());
    bool problem = st == EngineState::TabletProblem || st == EngineState::Error;
    std::wstring title = statusTitle(g.status);
    bool streaming = st == EngineState::Streaming;
    float textRight = streaming ? right - 28.0f : right;
    drawText(title, g.bodyFormat.Get(), D2D1::RectF(x + 16.0f, y, textRight, y + 22.0f), problem ? p.bad : p.text);
    g.infoRect = D2D1::RectF(0, 0, 0, 0);
    if (streaming) {
        g.infoRect = D2D1::RectF(right - 24.0f, y - 1.0f, right, y + 23.0f);
        if (g.hover == Focus::Info || g.showInfo) {
            brush->SetColor(p.hover);
            g.target->FillRoundedRectangle(D2D1::RoundedRect(g.infoRect, 4.0f, 4.0f), brush.Get());
        }
        drawText(L"", g.iconFormat.Get(), g.infoRect, p.secondary);
        if (g.keyboardFocus && g.focus == Focus::Info) {
            drawFocus(g.infoRect, 4.0f);
        }
    }
    y += 22.0f;
    std::wstring line = streamLine(g.status);
    if (!line.empty()) {
        drawText(line, g.captionFormat.Get(), D2D1::RectF(x + 16.0f, y, right, y + 20.0f), p.secondary);
        y += 20.0f;
    }
    if (g.showInfo && streaming) {
        drawText(infoLine(g.status), g.captionFormat.Get(), D2D1::RectF(x + 16.0f, y, right, y + 20.0f), p.tertiary);
        y += 20.0f;
    }
    y += 14.0f;
    brush->SetColor(p.divider);
    g.target->FillRectangle(D2D1::RectF(0.0f, y, kWidth, y + 1.0f), brush.Get());
    y += 1.0f + 6.0f;

    g.autoRowRect = D2D1::RectF(4.0f, y, kWidth - 4.0f, y + 40.0f);
    if (g.hover == Focus::Autostart) {
        brush->SetColor(p.hover);
        g.target->FillRoundedRectangle(D2D1::RoundedRect(g.autoRowRect, 4.0f, 4.0f), brush.Get());
    }
    drawText(L"Start with Windows", g.bodyFormat.Get(), D2D1::RectF(x, y, right - 48.0f, y + 40.0f), p.text);
    g.autoRect = D2D1::RectF(right - 40.0f, y + 10.0f, right, y + 30.0f);
    drawToggle(g.autoRect, g.autoKnob, g.hover == Focus::Autostart, g.keyboardFocus && g.focus == Focus::Autostart);

    if (g.target->EndDraw() == D2DERR_RECREATE_TARGET) {
        g.target.Reset();
    }
}

void startAnim() {
    if (animationsEnabled()) {
        SetTimer(g.hwnd, kAnimTimer, 15, nullptr);
    } else {
        g.powerKnob = g.enabled ? 1.0f : 0.0f;
        g.autoKnob = g.autostart ? 1.0f : 0.0f;
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void relayout() {
    float h = layoutHeight();
    if (std::fabs(h - g.height) > 0.5f && IsWindowVisible(g.hwnd)) {
        positionPanel();
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void togglePower() {
    g.enabled = !g.enabled;
    writeDword(L"Enabled", g.enabled ? 1 : 0);
    if (g.enabled) {
        startEngine();
    } else {
        stopEngineAsync();
    }
    g.status = g.engine ? g.engine->status() : EngineStatus{};
    updateTray(false);
    startAnim();
    relayout();
}

void toggleAutostart() {
    setAutostart(!g.autostart);
    startAnim();
}

void activate(Focus f) {
    if (f == Focus::Power) {
        togglePower();
    } else if (f == Focus::Autostart) {
        toggleAutostart();
    } else if (f == Focus::Info) {
        g.showInfo = !g.showInfo;
        relayout();
    }
}

bool inRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

Focus hitTest(LPARAM lp) {
    float scale = dpiScale();
    float x = static_cast<float>(static_cast<short>(LOWORD(lp))) / scale;
    float y = static_cast<float>(static_cast<short>(HIWORD(lp))) / scale;
    D2D1_RECT_F power = D2D1::RectF(g.powerRect.left - 6.0f, g.powerRect.top - 6.0f, g.powerRect.right + 6.0f, g.powerRect.bottom + 6.0f);
    if (inRect(power, x, y)) {
        return Focus::Power;
    }
    if (g.infoRect.right > 0 && inRect(g.infoRect, x, y)) {
        return Focus::Info;
    }
    if (inRect(g.autoRowRect, x, y)) {
        return Focus::Autostart;
    }
    return Focus::None;
}

void showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open TabDisplay");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == kMenuOpen) {
        showPanel();
    } else if (cmd == kMenuExit) {
        g.exiting = true;
        hidePanel();
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        if (g.engine && !g.stopping) {
            stopEngineAsync();
        } else if (!g.stopping) {
            PostQuitMessage(0);
        }
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreated && g.taskbarCreated != 0) {
        addTray();
        return 0;
    }
    switch (msg) {
        case kTrayMessage:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONUP:
                    if (IsWindowVisible(hwnd)) {
                        hidePanel();
                    } else {
                        showPanel();
                    }
                    return 0;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    showMenu();
                    return 0;
            }
            return 0;
        case kShowMessage:
            showPanel();
            return 0;
        case kStoppedMessage:
            if (g.stopper.joinable()) {
                g.stopper.join();
            }
            g.engine.reset();
            g.stopping = false;
            g.status = EngineStatus{};
            if (g.exiting) {
                PostQuitMessage(0);
                return 0;
            }
            if (g.enabled) {
                startEngine();
            }
            updateTray(false);
            relayout();
            return 0;
        case WM_TIMER:
            if (wp == kStatusTimer) {
                EngineStatus s = g.engine ? g.engine->status() : EngineStatus{};
                bool changed = s.state != g.status.state || s.detail != g.status.detail || s.width != g.status.width || s.fps != g.status.fps;
                bool metrics = std::fabs(s.rttMs - g.status.rttMs) > 0.05 || std::fabs(s.ackFps - g.status.ackFps) > 0.05;
                g.status = s;
                updateTray(false);
                if (IsWindowVisible(hwnd) && (changed || (metrics && g.showInfo))) {
                    relayout();
                }
            } else if (wp == kAnimTimer) {
                float targetPower = g.enabled ? 1.0f : 0.0f;
                float targetAuto = g.autostart ? 1.0f : 0.0f;
                auto step = [](float v, float t) { return std::fabs(t - v) < 0.02f ? t : v + (t - v) * 0.35f; };
                g.powerKnob = step(g.powerKnob, targetPower);
                g.autoKnob = step(g.autoKnob, targetAuto);
                if (g.powerKnob == targetPower && g.autoKnob == targetAuto) {
                    KillTimer(hwnd, kAnimTimer);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                hidePanel();
            }
            return 0;
        case WM_MOUSEMOVE: {
            Focus h = hitTest(lp);
            if (h != g.hover) {
                g.hover = h;
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g.hover = Focus::None;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            Focus h = hitTest(lp);
            if (h != Focus::None) {
                g.focus = h;
                g.keyboardFocus = false;
                activate(h);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                if (g.showInfo) {
                    g.showInfo = false;
                    relayout();
                } else {
                    hidePanel();
                }
            } else if (wp == VK_TAB) {
                bool back = GetKeyState(VK_SHIFT) < 0;
                bool hasInfo = g.infoRect.right > 0;
                Focus order[3] = {Focus::Power, Focus::Info, Focus::Autostart};
                int i = 0;
                for (int k = 0; k < 3; ++k) {
                    if (order[k] == g.focus) {
                        i = k;
                    }
                }
                do {
                    i = (i + (back ? 2 : 1)) % 3;
                } while (order[i] == Focus::Info && !hasInfo);
                g.focus = order[i];
                g.keyboardFocus = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wp == VK_SPACE || wp == VK_RETURN) {
                g.keyboardFocus = true;
                activate(g.focus);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (g.target) {
                g.target->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            }
            return 0;
        case WM_DPICHANGED:
            g.target.Reset();
            if (IsWindowVisible(hwnd)) {
                positionPanel();
            }
            updateTray(true);
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && std::wstring(reinterpret_cast<const wchar_t*>(lp)) == L"ImmersiveColorSet") {
                applyTheme();
                updateTray(true);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CLOSE:
            hidePanel();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!acquireSingleInstance()) {
        HANDLE show = OpenEventW(EVENT_MODIFY_STATE, FALSE, kShowEvent);
        if (show) {
            SetEvent(show);
            CloseHandle(show);
        }
        return 0;
    }
    HANDLE showEvent = CreateEventW(nullptr, FALSE, FALSE, kShowEvent);
    openLog();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    bool background = cmdLine && std::wstring(cmdLine).find(L"--background") != std::wstring::npos;

    g.instance = instance;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g.d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g.dwrite.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.wic));
    createFormats();
    loadSettings();

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    ATOM atom = RegisterClassExW(&wc);
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, L"TabDisplay", WS_POPUP, 0, 0, 320, 200, nullptr, nullptr, instance,
                             nullptr);
    if (!atom || !g.hwnd) {
        std::printf("[ui] window creation failed (%lu)\n", GetLastError());
        return 1;
    }
    if (showEvent) {
        HWND target = g.hwnd;
        std::thread([showEvent, target] {
            while (WaitForSingleObject(showEvent, INFINITE) == WAIT_OBJECT_0) {
                PostMessageW(target, kShowMessage, 0, 0);
            }
        }).detach();
    }
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    applyTheme();
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTray();
    if (g.enabled) {
        startEngine();
    }
    SetTimer(g.hwnd, kStatusTimer, 500, nullptr);
    if (!background) {
        showPanel();
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g.stopper.joinable()) {
        g.stopper.join();
    }
    if (g.engine) {
        g.engine->stop();
        g.engine.reset();
    }
    Shell_NotifyIconW(NIM_DELETE, &g.nid);
    if (g.icon) {
        DestroyIcon(g.icon);
    }
    CoUninitialize();
    return 0;
}
