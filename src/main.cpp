#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <shellapi.h>
#include <objbase.h>
#include <shellscalingapi.h>

#include <cmath>
#include <string>
#include <vector>

#include "brightness.h"

// ---------------------------------------------------------------------------
// 手工 GUID（MinGW 没有 __uuidof）
// ---------------------------------------------------------------------------
static const IID kIID_IDXGIDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const IID kIID_IDXGIFactory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const IID kIID_IDXGISurface = {0xcafcb56c, 0x6ac3, 0x4889, {0xbf, 0x47, 0x9e, 0x23, 0xbb, 0xd2, 0x60, 0xec}};
static const IID kIID_ID2D1Factory1 = {0xbb12d362, 0xdaee, 0x4b9a, {0xaa, 0x1d, 0x14, 0xba, 0x40, 0x1c, 0xfa, 0x1f}};
static const IID kIID_IDCompositionDevice = {0xc37ea93a, 0xe7aa, 0x450d, {0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3}};
static const IID kIID_IDWriteFactory = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
static const wchar_t* kClassName = L"LumaBrightWnd";
static const wchar_t* kOsdClass = L"LumaOsdWnd";
static const UINT WM_TRAY = WM_APP + 1;

static const UINT TIMER_POLL = 1;
static const UINT TIMER_FLUSH = 2;
static const UINT TIMER_OSD = 3;

static const UINT HK_COUNT = 3;
static const UINT HK_ID_BASE = 1;

static const float kWidth = 404.f;
static const float kPad = 12.f;
static const float kRowH = 74.f;
static const float kGap = 8.f;
static const float kFooterH = 34.f;

static const float kCardTop = 12.f;
static const float kCardH = 284.f;

// 迷你调节条（还原 Win11 原生浮层形态）
static const float kOsdW = 340.f;
static const float kOsdH = 68.f;
static const float kOsdRadius = 8.f;
static const float kOsdRow = 34.f;

static const D2D1_COLOR_F kText = {1.f, 1.f, 1.f, 0.94f};
static const D2D1_COLOR_F kTextDim = {1.f, 1.f, 1.f, 0.55f};
static const D2D1_COLOR_F kTextFaint = {1.f, 1.f, 1.f, 0.36f};
static const D2D1_COLOR_F kRowBg = {1.f, 1.f, 1.f, 0.045f};
static const D2D1_COLOR_F kRowHover = {1.f, 1.f, 1.f, 0.085f};
static const D2D1_COLOR_F kRowBorder = {1.f, 1.f, 1.f, 0.07f};
static const D2D1_COLOR_F kTrack = {1.f, 1.f, 1.f, 0.20f};
static const D2D1_COLOR_F kTrackLight = {1.f, 1.f, 1.f, 0.50f};
static const D2D1_COLOR_F kThumb = {1.f, 1.f, 1.f, 1.f};
static const D2D1_COLOR_F kBgFallback = {0.086f, 0.086f, 0.094f, 1.f};
static const D2D1_COLOR_F kOsdPanel = {0.173f, 0.176f, 0.180f, 0.97f};

// ---------------------------------------------------------------------------
// 设置
// ---------------------------------------------------------------------------
struct Hotkey {
    UINT mods = MOD_CONTROL | MOD_ALT;
    UINT vk = 0;
    bool enabled = true;
    bool ok = false;   // 注册是否成功
};

struct Settings {
    Hotkey hk[HK_COUNT];
    int osdPos = 1;    // 0 左上 1 上中 2 右上 3 左下 4 下中 5 右下
    int osdSel = -1;   // -1 全部显示器，否则为显示器序号
    bool autostart = false;
};
static Settings g_set;

static void InitDefaults() {
    g_set.hk[0].mods = MOD_CONTROL | MOD_ALT;
    g_set.hk[0].vk = VK_UP;
    g_set.hk[1].mods = MOD_CONTROL | MOD_ALT;
    g_set.hk[1].vk = VK_DOWN;
    g_set.hk[2].mods = MOD_CONTROL | MOD_ALT;
    g_set.hk[2].vk = 'B';
    for (UINT i = 0; i < HK_COUNT; ++i) g_set.hk[i].enabled = true;
}

static std::wstring SettingsDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    return std::wstring(buf) + L"\\Luma";
}

static std::wstring SettingsFile() { return SettingsDir() + L"\\settings.ini"; }

static void LoadSettings() {
    InitDefaults();
    std::wstring f = SettingsFile();
    for (UINT i = 0; i < HK_COUNT; ++i) {
        wchar_t key[8];
        swprintf(key, 8, L"hk%u", i + 1);
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"Luma", key, L"", buf, 64, f.c_str());
        int en = 1, mods = 0, vk = 0;
        if (swscanf(buf, L"%d,%d,%d", &en, &mods, &vk) == 3 && vk != 0) {
            g_set.hk[i].enabled = en != 0;
            g_set.hk[i].mods = (UINT)mods;
            g_set.hk[i].vk = (UINT)vk;
        }
    }
    int pos = GetPrivateProfileIntW(L"Luma", L"osdpos", 1, f.c_str());
    if (pos < 0 || pos > 5) pos = 1;
    g_set.osdPos = pos;
    g_set.osdSel = GetPrivateProfileIntW(L"Luma", L"osdsel", -1, f.c_str());
}

static void SaveSettings() {
    CreateDirectoryW(SettingsDir().c_str(), nullptr);
    std::wstring f = SettingsFile();
    for (UINT i = 0; i < HK_COUNT; ++i) {
        wchar_t key[8], val[64];
        swprintf(key, 8, L"hk%u", i + 1);
        swprintf(val, 64, L"%d,%d,%d", g_set.hk[i].enabled ? 1 : 0, (int)g_set.hk[i].mods, (int)g_set.hk[i].vk);
        WritePrivateProfileStringW(L"Luma", key, val, f.c_str());
    }
    wchar_t v[8];
    swprintf(v, 8, L"%d", g_set.osdPos);
    WritePrivateProfileStringW(L"Luma", L"osdpos", v, f.c_str());
    swprintf(v, 8, L"%d", g_set.osdSel);
    WritePrivateProfileStringW(L"Luma", L"osdsel", v, f.c_str());
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static bool IsAutoStart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = sizeof(buf) - 2;
    DWORD type = 0;
    bool exists = RegQueryValueExW(key, L"Luma", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

static void SetAutoStart(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(key, L"Luma", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"Luma");
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static HINSTANCE g_inst = nullptr;
static HWND g_hwnd = nullptr;
static HWND g_osd = nullptr;
static float g_scale = 1.f;
static float g_osdScale = 1.f;
static float g_clientH = 202.f;
static bool g_mica = false;
static D2D1_COLOR_F g_accent = {0.30f, 0.76f, 1.f, 1.f};
static bool g_settingsView = false;
static int g_captureHk = -1;   // >=0 表示正在录入该序号的快捷键

static ID3D11Device* g_d3d = nullptr;
static ID3D11DeviceContext* g_d3dCtx = nullptr;
static IDXGIFactory2* g_dxgiFactory = nullptr;
static IDCompositionDevice* g_compDevice = nullptr;
static ID2D1Factory1* g_d2dFactory = nullptr;
static ID2D1Device* g_d2dDevice = nullptr;
static ID2D1DeviceContext* g_dc = nullptr;
static ID2D1SolidColorBrush* g_brush = nullptr;
static ID2D1StrokeStyle* g_roundStroke = nullptr;
static IDWriteFactory* g_writeFactory = nullptr;
static IDWriteTextFormat* g_tfName = nullptr;
static IDWriteTextFormat* g_tfPct = nullptr;
static IDWriteTextFormat* g_tfSmall = nullptr;
static IDWriteTextFormat* g_tfTiny = nullptr;
static IDWriteTextFormat* g_tfBig = nullptr;
static IDWriteInlineObject* g_ellipsis = nullptr;

struct Surface {
    IDXGISwapChain1* sc = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    ID2D1Bitmap1* bmp = nullptr;
    float scale = 1.f;
};
static Surface g_sMain;
static Surface g_sOsd;

struct RowBox {
    float top = 0;
    int index = 0;
};
static std::vector<RowBox> g_rows;

static int g_hoverRow = -1;
static bool g_hoverSlider = false;
static int g_dragRow = -1;
static bool g_trackingLeave = false;
static int g_hoverCtl = -1;   // 设置页：0..2 快捷键开关 3 自启 10..15 位置 20 返回 30..32 快捷键按钮

static int g_osdPct = 50;
static bool g_osdDrag = false;
static bool g_osdList = false;
static int g_osdHoverItem = -1;
static bool g_osdChevronHover = false;
static DWORD g_osdLastWrite = 0;

static int g_pendingVal[16] = {};
static bool g_pending[16] = {};
static DWORD g_lastWrite[16] = {};

static NOTIFYICONDATAW g_nid = {};
static bool g_trayAdded = false;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static D2D1_COLOR_F Lighten(D2D1_COLOR_F c, float amount) {
    D2D1_COLOR_F o = c;
    o.r = c.r + (1.f - c.r) * amount;
    o.g = c.g + (1.f - c.g) * amount;
    o.b = c.b + (1.f - c.b) * amount;
    return o;
}

static D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

static std::wstring KeyName(UINT vk) {
    switch (vk) {
        case VK_UP: return L"↑";
        case VK_DOWN: return L"↓";
        case VK_LEFT: return L"←";
        case VK_RIGHT: return L"→";
        case VK_SPACE: return L"Space";
        case VK_TAB: return L"Tab";
        case VK_RETURN: return L"Enter";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"PgUp";
        case VK_NEXT: return L"PgDn";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_OEM_PLUS: return L"=";
        case VK_OEM_MINUS: return L"-";
        case VK_OEM_COMMA: return L",";
        case VK_OEM_PERIOD: return L".";
        case VK_OEM_1: return L";";
        case VK_OEM_2: return L"/";
        case VK_OEM_3: return L"`";
        case VK_OEM_4: return L"[";
        case VK_OEM_5: return L"\\";
        case VK_OEM_6: return L"]";
        case VK_OEM_7: return L"'";
        default: break;
    }
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    wchar_t buf[32] = {};
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    if (GetKeyNameTextW((LONG)sc, buf, 32) > 0) return buf;
    return L"?";
}

static std::wstring ComboText(const Hotkey& hk) {
    std::wstring s;
    if (hk.mods & MOD_CONTROL) s += L"Ctrl + ";
    if (hk.mods & MOD_SHIFT) s += L"Shift + ";
    if (hk.mods & MOD_ALT) s += L"Alt + ";
    if (hk.mods & MOD_WIN) s += L"Win + ";
    s += KeyName(hk.vk);
    return s;
}

static float CalcClientHeight() {
    if (g_settingsView) return kCardTop + kCardH + 8.f + kFooterH;
    int n = (int)BrightList().size();
    if (n <= 0) return kPad + 84.f + kFooterH;
    return kPad + n * kRowH + (n - 1) * kGap + kFooterH;
}

// 列表里第一行是“全部显示器”（仅当有两块以上屏），其余为各显示器
static int OsdRowCount() {
    int n = (int)BrightList().size();
    return n > 1 ? n + 1 : n;
}

static int OsdRowToSel(int row) {
    int n = (int)BrightList().size();
    if (n > 1) return row == 0 ? -1 : row - 1;
    return row == 0 ? 0 : -1;
}

static int SelToRow(int sel) {
    int n = (int)BrightList().size();
    if (n > 1) return sel < 0 ? 0 : sel + 1;
    return sel == 0 ? 0 : -1;
}

static const wchar_t* OsdRowName(int row) {
    int n = (int)BrightList().size();
    if (n > 1 && row == 0) return L"全部显示器";
    int idx = (n > 1) ? row - 1 : row;
    if (idx < 0 || idx >= n) return L"";
    return BrightList()[idx].name.c_str();
}

static float OsdHeight() {
    if (!g_osdList) return kOsdH;
    return kOsdH + 6.f + OsdRowCount() * kOsdRow + 6.f;
}

static void LoadAccent() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr,
                     &v, &sz) == ERROR_SUCCESS) {
        float r = (float)(v & 0xFF) / 255.f;
        float g = (float)((v >> 8) & 0xFF) / 255.f;
        float b = (float)((v >> 16) & 0xFF) / 255.f;
        if (r + g + b > 0.12f) {
            D2D1_COLOR_F c = {r, g, b, 1.f};
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (lum < 0.45f) c = Lighten(c, 0.35f);
            g_accent = c;
            return;
        }
    }
    D2D1_COLOR_F def = {0.29f, 0.76f, 1.f, 1.f};
    g_accent = def;
}

static IDWriteTextFormat* MakeFormat(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat* f = nullptr;
    if (FAILED(g_writeFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &f)))
        return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return f;
}

// ---------------------------------------------------------------------------
// 图形
// ---------------------------------------------------------------------------
static void DestroySurface(Surface& s) {
    if (s.bmp) { s.bmp->Release(); s.bmp = nullptr; }
    if (s.target) { s.target->Release(); s.target = nullptr; }
    if (s.visual) { s.visual->Release(); s.visual = nullptr; }
    if (s.sc) { s.sc->Release(); s.sc = nullptr; }
}

static void ReleaseGraphics() {
    if (g_dc) g_dc->SetTarget(nullptr);
    DestroySurface(g_sMain);
    DestroySurface(g_sOsd);
    if (g_brush) { g_brush->Release(); g_brush = nullptr; }
    if (g_dc) { g_dc->Release(); g_dc = nullptr; }
    if (g_d2dDevice) { g_d2dDevice->Release(); g_d2dDevice = nullptr; }
    if (g_d2dFactory) { g_d2dFactory->Release(); g_d2dFactory = nullptr; }
    if (g_roundStroke) { g_roundStroke->Release(); g_roundStroke = nullptr; }
    if (g_compDevice) { g_compDevice->Release(); g_compDevice = nullptr; }
    if (g_dxgiFactory) { g_dxgiFactory->Release(); g_dxgiFactory = nullptr; }
    if (g_d3dCtx) { g_d3dCtx->Release(); g_d3dCtx = nullptr; }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
}

static bool BindSurface(Surface& s) {
    if (!g_dc || !s.bmp) return false;
    g_dc->SetTarget(s.bmp);
    g_dc->SetDpi(96.f * s.scale, 96.f * s.scale);
    g_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

static bool MakeTargetBitmap(Surface& s) {
    IDXGISurface* surface = nullptr;
    if (FAILED(s.sc->GetBuffer(0, kIID_IDXGISurface, (void**)&surface)) || !surface) return false;
    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    props.dpiX = 96.f * s.scale;
    props.dpiY = 96.f * s.scale;
    HRESULT hr = g_dc->CreateBitmapFromDxgiSurface(surface, &props, &s.bmp);
    surface->Release();
    return SUCCEEDED(hr);
}

static bool CreateSurface(Surface& s, HWND hwnd, UINT w, UINT h) {
    if (!g_dxgiFactory || !g_compDevice || !g_dc) return false;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = w < 1 ? 1 : w;
    desc.Height = h < 1 ? 1 : h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(g_dxgiFactory->CreateSwapChainForComposition(g_d3d, &desc, nullptr, &s.sc)) || !s.sc) return false;
    if (FAILED(g_compDevice->CreateTargetForHwnd(hwnd, TRUE, &s.target)) || !s.target) return false;
    if (FAILED(g_compDevice->CreateVisual(&s.visual)) || !s.visual) return false;
    s.visual->SetContent(s.sc);
    s.target->SetRoot(s.visual);
    g_compDevice->Commit();
    return MakeTargetBitmap(s);
}

static void ResizeSurface(Surface& s, UINT w, UINT h) {
    if (!s.sc || w < 1 || h < 1) return;
    if (g_dc) g_dc->SetTarget(nullptr);
    if (s.bmp) { s.bmp->Release(); s.bmp = nullptr; }
    if (FAILED(s.sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return;
    MakeTargetBitmap(s);
}

static bool CreateGraphics() {
    LoadAccent();

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, kIID_IDWriteFactory, (IUnknown**)&g_writeFactory);
    if (!g_writeFactory) return false;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, kIID_ID2D1Factory1, nullptr, (void**)&g_d2dFactory)))
        return false;

    g_tfName = MakeFormat(13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_tfPct = MakeFormat(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfSmall = MakeFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_tfTiny = MakeFormat(11.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfBig = MakeFormat(20.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (g_tfName && g_writeFactory->CreateEllipsisTrimmingSign(g_tfName, &g_ellipsis) == S_OK && g_ellipsis) {
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        g_tfName->SetTrimming(&trim, g_ellipsis);
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_1};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 5, D3D11_SDK_VERSION,
                                   &g_d3d, nullptr, &g_d3dCtx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 5, D3D11_SDK_VERSION, &g_d3d,
                               nullptr, &g_d3dCtx);
        if (FAILED(hr)) return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(g_d3d->QueryInterface(kIID_IDXGIDevice, (void**)&dxgiDevice)) || !dxgiDevice) return false;

    if (FAILED(g_d2dFactory->CreateDevice(dxgiDevice, &g_d2dDevice)) || !g_d2dDevice) {
        dxgiDevice->Release();
        return false;
    }
    if (FAILED(g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc)) || !g_dc) {
        dxgiDevice->Release();
        return false;
    }
    if (FAILED(g_dc->CreateSolidColorBrush(kText, &g_brush)) || !g_brush) {
        dxgiDevice->Release();
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    if (!adapter) {
        dxgiDevice->Release();
        return false;
    }
    HRESULT hf = adapter->GetParent(kIID_IDXGIFactory2, (void**)&g_dxgiFactory);
    adapter->Release();
    if (FAILED(hf) || !g_dxgiFactory) {
        dxgiDevice->Release();
        return false;
    }

    if (FAILED(DCompositionCreateDevice(dxgiDevice, kIID_IDCompositionDevice, (void**)&g_compDevice)) || !g_compDevice) {
        dxgiDevice->Release();
        return false;
    }
    dxgiDevice->Release();

    g_sMain.scale = g_scale;
    g_sOsd.scale = g_osdScale;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (!CreateSurface(g_sMain, g_hwnd, (UINT)rc.right, (UINT)(rc.bottom > 0 ? rc.bottom : 1))) return false;

    GetClientRect(g_osd, &rc);
    if (!CreateSurface(g_sOsd, g_osd, (UINT)rc.right, (UINT)(rc.bottom > 0 ? rc.bottom : 1))) return false;

    D2D1_STROKE_STYLE_PROPERTIES ssp = {};
    ssp.startCap = D2D1_CAP_STYLE_ROUND;
    ssp.endCap = D2D1_CAP_STYLE_ROUND;
    g_d2dFactory->CreateStrokeStyle(ssp, nullptr, 0, &g_roundStroke);
    return true;
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
static void ComputeRows() {
    g_rows.clear();
    int n = (int)BrightList().size();
    float y = kPad;
    for (int i = 0; i < n; ++i) {
        RowBox rb;
        rb.top = y;
        rb.index = i;
        g_rows.push_back(rb);
        y += kRowH + kGap;
    }
}

static void UpdateWindowSize() {
    g_clientH = CalcClientHeight();
    int cw = (int)lroundf(kWidth * g_scale);
    int ch = (int)lroundf(g_clientH * g_scale);

    DWORD style = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    RECT r = {0, 0, cw, ch};
    UINT dpi = (UINT)(96.f * g_scale + 0.5f);
    if (!AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, dpi)) AdjustWindowRectEx(&r, style, FALSE, exStyle);

    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    SetWindowPos(g_hwnd, nullptr, wr.left, wr.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    ComputeRows();
}

struct CfgLayout {
    D2D1_RECT_F hkToggle[3];
    D2D1_RECT_F hkChip[3];
    D2D1_RECT_F autoToggle;
    D2D1_RECT_F pos[6];
    D2D1_RECT_F footerBtn;
};

static const wchar_t* kPosNames[6] = {L"左上", L"上中", L"右上", L"左下", L"下中", L"右下"};
static const wchar_t* kHkNames[3] = {L"提高亮度", L"降低亮度", L"亮度调节条"};

static CfgLayout LayoutCfg() {
    CfgLayout L;
    float ys[3] = {44.f, 78.f, 112.f};
    for (int i = 0; i < 3; ++i) {
        L.hkChip[i] = {156.f, ys[i] + 2.f, 300.f, ys[i] + 28.f};
        L.hkToggle[i] = {332.f, ys[i] + 4.f, 372.f, ys[i] + 24.f};
    }
    L.autoToggle = {332.f, 158.f, 372.f, 178.f};
    float xs[3] = {26.f, 145.f, 264.f};
    float gy[2] = {218.f, 252.f};
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c) L.pos[r * 3 + c] = {xs[c], gy[r], xs[c] + 111.f, gy[r] + 30.f};
    L.footerBtn = {kWidth - 68.f, g_clientH - kFooterH + 3.f, kWidth - 12.f, g_clientH - 3.f};
    return L;
}

static float OsdX1() { return 58.f; }
static float OsdX2() { return 266.f; }

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
static void FillRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
    g_brush->SetColor(c);
    D2D1_ROUNDED_RECT rr = {r, radius, radius};
    g_dc->FillRoundedRectangle(&rr, g_brush);
}

static void StrokeRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float width) {
    g_brush->SetColor(c);
    D2D1_ROUNDED_RECT rr = {r, radius, radius};
    g_dc->DrawRoundedRectangle(&rr, g_brush, width, nullptr);
}

static void TextIn(const wchar_t* s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    g_brush->SetColor(c);
    g_dc->DrawTextW(s, (UINT32)wcslen(s), f, &r, g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
}

static void DrawSun(float cx, float cy, float r, const D2D1_COLOR_F& c) {
    g_brush->SetColor(c);
    D2D1_ELLIPSE e = {D2D1_POINT_2F{cx, cy}, r * 0.36f, r * 0.36f};
    g_dc->FillEllipse(&e, g_brush);
    const float pi = 3.14159265358979f;
    for (int i = 0; i < 8; ++i) {
        float a = (float)i * pi / 4.f;
        float dx = cosf(a), dy = sinf(a);
        D2D1_POINT_2F p1 = {cx + dx * r * 0.68f, cy + dy * r * 0.68f};
        D2D1_POINT_2F p2 = {cx + dx * r * 0.98f, cy + dy * r * 0.98f};
        g_dc->DrawLine(p1, p2, g_brush, 1.05f, g_roundStroke);
    }
}

static void DrawChevron(float cx, float cy, bool up, const D2D1_COLOR_F& c) {
    g_brush->SetColor(c);
    float w = 5.f, h = 3.f;
    if (up) {
        g_dc->DrawLine(D2D1_POINT_2F{cx - w, cy + h}, D2D1_POINT_2F{cx, cy - h}, g_brush, 1.3f, g_roundStroke);
        g_dc->DrawLine(D2D1_POINT_2F{cx, cy - h}, D2D1_POINT_2F{cx + w, cy + h}, g_brush, 1.3f, g_roundStroke);
    } else {
        g_dc->DrawLine(D2D1_POINT_2F{cx - w, cy - h}, D2D1_POINT_2F{cx, cy + h}, g_brush, 1.3f, g_roundStroke);
        g_dc->DrawLine(D2D1_POINT_2F{cx, cy + h}, D2D1_POINT_2F{cx + w, cy - h}, g_brush, 1.3f, g_roundStroke);
    }
}

static void DrawToggle(const D2D1_RECT_F& r, bool on, bool hover) {
    float h = r.bottom - r.top;
    D2D1_COLOR_F track = on ? (hover ? Lighten(g_accent, 0.12f) : g_accent) : WithAlpha(kThumb, hover ? 0.20f : 0.13f);
    FillRound(r, h * 0.5f, track);
    float knob = h - 6.f;
    float cx = on ? (r.right - 3.f - knob * 0.5f) : (r.left + 3.f + knob * 0.5f);
    g_brush->SetColor(on ? D2D1_COLOR_F{1.f, 1.f, 1.f, 1.f} : D2D1_COLOR_F{1.f, 1.f, 1.f, 0.85f});
    g_dc->FillEllipse(D2D1_ELLIPSE{D2D1_POINT_2F{cx, (r.top + r.bottom) * 0.5f}, knob * 0.5f, knob * 0.5f}, g_brush);
}

static void DrawRow(const RowBox& rb, const CtDisplay& d) {
    D2D1_RECT_F r = {kPad, rb.top, kWidth - kPad, rb.top + kRowH};
    bool hover = (g_hoverRow == rb.index);

    FillRound(r, 10.f, hover ? kRowHover : kRowBg);
    StrokeRound(r, 10.f, kRowBorder, 1.f);
    DrawSun(r.left + 24.f, r.top + 24.f, 8.5f, d.controllable ? kTextDim : kTextFaint);

    D2D1_RECT_F nameR = {r.left + 42.f, r.top + 13.f, r.right - 66.f, r.top + 35.f};
    TextIn(d.name.c_str(), g_tfName, nameR, d.controllable ? kText : kTextFaint);

    if (!d.controllable) {
        D2D1_RECT_F subR = {r.left + 42.f, r.top + 34.f, r.right - 16.f, r.top + 54.f};
        TextIn(d.sub.c_str(), g_tfSmall, subR, kTextFaint);
        return;
    }

    wchar_t buf[32];
    swprintf(buf, 32, L"%d%%", BrightPercent(rb.index));
    D2D1_RECT_F pctR = {r.right - 62.f, r.top + 13.f, r.right - 16.f, r.top + 35.f};
    TextIn(buf, g_tfPct, pctR, hover ? kText : kTextDim);

    float x1 = r.left + 16.f;
    float x2 = r.right - 16.f;
    float cy = r.top + 53.f;
    float t = (d.max > d.min) ? (float)(d.cur - d.min) / (float)(d.max - d.min) : 0.f;
    t = Clamp01(t);
    float tx = x1 + t * (x2 - x1);

    FillRound({x1, cy - 2.f, x2, cy + 2.f}, 2.f, kTrack);
    if (tx > x1 + 0.5f) FillRound({x1, cy - 2.f, tx, cy + 2.f}, 2.f, g_accent);

    float tr = 7.f;
    if (g_dragRow == rb.index) tr = 8.5f;
    else if (hover && g_hoverSlider) tr = 8.f;
    g_brush->SetColor(kThumb);
    g_dc->FillEllipse(D2D1_ELLIPSE{D2D1_POINT_2F{tx, cy}, tr, tr}, g_brush);
}

static void DrawFooter() {
    float top = g_clientH - kFooterH;
    const auto& list = BrightList();
    bool anyCtl = false;
    for (const auto& d : list)
        if (d.controllable) { anyCtl = true; break; }

    std::wstring hint;
    if (g_settingsView) {
        hint = L"设置会自动保存";
    } else if (list.empty() || !anyCtl) {
        hint = L"读不到亮度，按 F5 重新扫描";
    } else {
        hint = L"滚轮调节  ·  快捷键可在设置里改";
    }
    TextIn(hint.c_str(), g_tfSmall, {20.f, top + 6.f, kWidth - 80.f, top + 28.f}, kTextFaint);

    D2D1_RECT_F btn = {kWidth - 68.f, top + 3.f, kWidth - 12.f, top + 31.f};
    bool hover = (g_hoverCtl == 20);
    if (hover) FillRound(btn, 6.f, {1.f, 1.f, 1.f, 0.07f});
    if (g_settingsView) {
        TextIn(L"返回", g_tfSmall, btn, hover ? kText : kTextDim);
    } else {
        float cx = (btn.left + btn.right) * 0.5f;
        float cy = (btn.top + btn.bottom) * 0.5f;
        g_brush->SetColor(hover ? kText : kTextDim);
        for (int i = 0; i < 3; ++i)
            g_dc->DrawLine(D2D1_POINT_2F{cx - 7.f, cy - 6.f + i * 6.f}, D2D1_POINT_2F{cx + 7.f, cy - 6.f + i * 6.f},
                           g_brush, 1.2f, g_roundStroke);
        g_dc->FillEllipse(D2D1_ELLIPSE{D2D1_POINT_2F{cx - 3.f, cy - 6.f}, 2.2f, 2.2f}, g_brush);
        g_dc->FillEllipse(D2D1_ELLIPSE{D2D1_POINT_2F{cx + 4.f, cy}, 2.2f, 2.2f}, g_brush);
        g_dc->FillEllipse(D2D1_ELLIPSE{D2D1_POINT_2F{cx - 1.f, cy + 6.f}, 2.2f, 2.2f}, g_brush);
    }
}

static void DrawCfg() {
    CfgLayout L = LayoutCfg();
    FillRound({kPad, kCardTop, kWidth - kPad, kCardTop + kCardH}, 10.f, kRowBg);
    StrokeRound({kPad, kCardTop, kWidth - kPad, kCardTop + kCardH}, 10.f, kRowBorder, 1.f);

    TextIn(L"快捷键", g_tfSmall, {26.f, 20.f, 200.f, 40.f}, kTextDim);

    float ys[3] = {44.f, 78.f, 112.f};
    for (int i = 0; i < 3; ++i) {
        TextIn(kHkNames[i], g_tfName, {26.f, ys[i], 150.f, ys[i] + 30.f}, kText);
        // 快捷键按钮
        bool capturing = (g_captureHk == i);
        bool chipHover = (g_hoverCtl == 30 + i);
        D2D1_COLOR_F chipBg = capturing ? WithAlpha(g_accent, 0.20f)
                                        : (chipHover ? D2D1_COLOR_F{1.f, 1.f, 1.f, 0.10f} : D2D1_COLOR_F{1.f, 1.f, 1.f, 0.05f});
        FillRound(L.hkChip[i], 6.f, chipBg);
        if (capturing) StrokeRound(L.hkChip[i], 6.f, WithAlpha(g_accent, 0.8f), 1.f);
        std::wstring txt = capturing ? L"按下新的快捷键…" : ComboText(g_set.hk[i]);
        D2D1_COLOR_F tc = capturing ? kText : (g_set.hk[i].enabled ? kTextDim : kTextFaint);
        if (!capturing && g_set.hk[i].enabled && !g_set.hk[i].ok) tc = {1.f, 0.55f, 0.45f, 0.95f};
        TextIn(txt.c_str(), g_tfTiny, L.hkChip[i], tc);
        DrawToggle(L.hkToggle[i], g_set.hk[i].enabled, g_hoverCtl == (int)i);
    }

    g_brush->SetColor({1.f, 1.f, 1.f, 0.07f});
    g_dc->DrawLine(D2D1_POINT_2F{26.f, 150.f}, D2D1_POINT_2F{378.f, 150.f}, g_brush, 1.f);

    TextIn(L"开机自启", g_tfName, {26.f, 156.f, 200.f, 180.f}, kText);
    DrawToggle(L.autoToggle, g_set.autostart, g_hoverCtl == 3);

    g_brush->SetColor({1.f, 1.f, 1.f, 0.07f});
    g_dc->DrawLine(D2D1_POINT_2F{26.f, 188.f}, D2D1_POINT_2F{378.f, 188.f}, g_brush, 1.f);

    TextIn(L"调节条出现位置", g_tfSmall, {26.f, 194.f, 300.f, 214.f}, kTextDim);
    for (int i = 0; i < 6; ++i) {
        bool sel = (g_set.osdPos == i);
        bool hover = (g_hoverCtl == 10 + i);
        D2D1_COLOR_F bg = sel ? WithAlpha(g_accent, 0.28f) : (hover ? D2D1_COLOR_F{1.f, 1.f, 1.f, 0.09f}
                                                                   : D2D1_COLOR_F{1.f, 1.f, 1.f, 0.05f});
        FillRound(L.pos[i], 8.f, bg);
        if (sel) StrokeRound(L.pos[i], 8.f, WithAlpha(g_accent, 0.75f), 1.f);
        TextIn(kPosNames[i], g_tfTiny, L.pos[i], sel ? kText : kTextDim);
    }
}

static void RenderMain() {
    if (!BindSurface(g_sMain)) return;
    g_dc->BeginDraw();
    if (g_mica) {
        D2D1_COLOR_F clear = {0.f, 0.f, 0.f, 0.f};
        g_dc->Clear(&clear);
    } else {
        g_dc->Clear(&kBgFallback);
    }

    if (g_settingsView) {
        DrawCfg();
    } else {
        const auto& list = BrightList();
        if (list.empty()) {
            TextIn(L"未检测到显示器", g_tfName, {kPad, kPad + 20.f, kWidth - kPad, kPad + 50.f}, kTextDim);
        } else {
            for (const auto& rb : g_rows) DrawRow(rb, list[rb.index]);
        }
    }
    DrawFooter();

    HRESULT hr = g_dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseGraphics();
        CreateGraphics();
        RenderMain();
        return;
    }
    g_sMain.sc->Present(1, 0);
}

static void RenderOsd() {
    if (!BindSurface(g_sOsd)) return;
    g_dc->BeginDraw();
    D2D1_COLOR_F clear = {0.f, 0.f, 0.f, 0.f};
    g_dc->Clear(&clear);

    float w = kOsdW;
    float totalH = OsdHeight();
    FillRound({0.f, 0.f, w, totalH}, kOsdRadius, kOsdPanel);
    StrokeRound({0.5f, 0.5f, w - 0.5f, totalH - 0.5f}, kOsdRadius, {1.f, 1.f, 1.f, 0.07f}, 1.f);

    float cy = kOsdH * 0.5f;
    DrawSun(28.f, cy, 9.f, kText);

    float x1 = OsdX1(), x2 = OsdX2();
    float t = Clamp01(g_osdPct / 100.f);
    float tx = x1 + t * (x2 - x1);

    FillRound({x1, cy - 2.5f, x2, cy + 2.5f}, 2.5f, kTrackLight);
    if (tx > x1 + 0.5f) FillRound({x1, cy - 2.5f, tx, cy + 2.5f}, 2.5f, g_accent);

    wchar_t buf[16];
    swprintf(buf, 16, L"%d", g_osdPct);
    TextIn(buf, g_tfBig, {x2 + 6.f, 0.f, w - 42.f, kOsdH}, kText);

    if (g_osdChevronHover) DrawChevron(kOsdW - 24.f, cy, g_osdList, kText);
    else DrawChevron(kOsdW - 24.f, cy, g_osdList, kTextDim);

    if (g_osdList) {
        float y = kOsdH + 6.f;
        int rows = OsdRowCount();
        for (int i = 0; i < rows; ++i) {
            D2D1_RECT_F row = {10.f, y, w - 10.f, y + kOsdRow};
            int sel = OsdRowToSel(i);
            bool isSel = (g_set.osdSel == sel);
            if (g_osdHoverItem == i) FillRound(row, 6.f, {1.f, 1.f, 1.f, 0.07f});
            if (isSel) FillRound(row, 6.f, WithAlpha(g_accent, 0.16f));
            D2D1_RECT_F tr = {26.f, y, w - 50.f, y + kOsdRow};
            TextIn(OsdRowName(i), g_tfName, tr, isSel ? kText : kTextDim);
            if (isSel) {
                g_brush->SetColor(g_accent);
                float cx = w - 30.f, ccy = y + kOsdRow * 0.5f;
                g_dc->DrawLine(D2D1_POINT_2F{cx - 5.f, ccy}, D2D1_POINT_2F{cx - 1.f, ccy + 4.f}, g_brush, 1.8f,
                               g_roundStroke);
                g_dc->DrawLine(D2D1_POINT_2F{cx - 1.f, ccy + 4.f}, D2D1_POINT_2F{cx + 5.f, ccy - 4.f}, g_brush, 1.8f,
                               g_roundStroke);
            }
            y += kOsdRow;
        }
    }

    HRESULT hr = g_dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseGraphics();
        CreateGraphics();
        RenderOsd();
        return;
    }
    g_sOsd.sc->Present(1, 0);
}

// ---------------------------------------------------------------------------
// 调光
// ---------------------------------------------------------------------------
static void FlushPending(int idx) {
    if (idx < 0 || idx >= 16 || !g_pending[idx]) return;
    g_pending[idx] = false;
    BrightSet(idx, g_pendingVal[idx]);
    g_lastWrite[idx] = GetTickCount();
}

static void RequestSet(int idx, int value) {
    if (idx < 0 || idx >= 16 || idx >= (int)BrightList().size()) return;
    if (!BrightList()[idx].controllable) return;
    g_pendingVal[idx] = value;
    g_pending[idx] = true;
    if (GetTickCount() - g_lastWrite[idx] >= 22) {
        FlushPending(idx);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    } else {
        SetTimer(g_hwnd, TIMER_FLUSH, 20, nullptr);
    }
}

static int ValueFromPos(int idx, float x) {
    const auto& d = BrightList()[idx];
    float x1 = kPad + 16.f;
    float x2 = kWidth - kPad - 16.f;
    float t = Clamp01((x - x1) / (x2 - x1));
    return d.min + (int)lroundf(t * (float)(d.max - d.min));
}

// 内屏走 WMI，单次写入较慢，拖动时按 180ms 节流，松手后再补最后一次
static DWORD g_lastWmiWrite = 0;
static int g_pendingWmiPct = -1;

static void FlushWmiPending() {
    if (g_pendingWmiPct < 0) return;
    int pct = g_pendingWmiPct;
    g_pendingWmiPct = -1;
    const auto& list = BrightList();
    for (size_t i = 0; i < list.size(); ++i) {
        if (!list[i].controllable || !list[i].internal) continue;
        BrightSet((int)i, list[i].min + (list[i].max - list[i].min) * pct / 100);
    }
    g_lastWmiWrite = GetTickCount();
}

// 取当前目标显示器（osdSel >= 0）或全部
static void SetPercent(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    DWORD now = GetTickCount();
    const auto& list = BrightList();
    for (size_t i = 0; i < list.size(); ++i) {
        if (!list[i].controllable) continue;
        if (g_set.osdSel >= 0 && (int)i != g_set.osdSel) continue;
        int v = list[i].min + (list[i].max - list[i].min) * pct / 100;
        if (list[i].internal) {
            if (now - g_lastWmiWrite >= 180) {
                BrightSet((int)i, v);
                g_lastWmiWrite = now;
                g_pendingWmiPct = -1;
            } else {
                g_pendingWmiPct = pct;
            }
        } else {
            BrightSet((int)i, v);
        }
    }
    g_osdPct = pct;
}

static int TargetPercent() {
    const auto& list = BrightList();
    if (g_set.osdSel >= 0 && g_set.osdSel < (int)list.size() && list[g_set.osdSel].controllable)
        return BrightPercent(g_set.osdSel);
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].controllable) return BrightPercent((int)i);
    return 0;
}

static void AdjustBy(int delta) {
    SetPercent(TargetPercent() + delta);
    if (g_trayAdded) {
        swprintf(g_nid.szTip, 128, L"亮度 %d%%", g_osdPct);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
    if (IsWindowVisible(g_osd)) InvalidateRect(g_osd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 迷你调节条
// ---------------------------------------------------------------------------
static void PlaceOsd() {
    if (!g_osd) return;

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return;

    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    g_osdScale = (float)dx / 96.f;

    int w = (int)lroundf(kOsdW * g_osdScale);
    int h = (int)lroundf(OsdHeight() * g_osdScale);
    int hm2 = (int)lroundf(kOsdH * g_osdScale);
    int m = (int)lroundf(24.f * g_osdScale);
    RECT wa = mi.rcWork;
    int x = wa.left + m, y = wa.top + m;
    switch (g_set.osdPos) {
        case 0: x = wa.left + m; y = wa.top + m; break;
        case 1: x = (wa.left + wa.right - w) / 2; y = wa.top + m; break;
        case 2: x = wa.right - w - m; y = wa.top + m; break;
        case 3: x = wa.left + m; y = wa.bottom - h - m; break;
        case 4: x = (wa.left + wa.right - w) / 2; y = wa.bottom - h - m; break;
        default: x = wa.right - w - m; y = wa.bottom - h - m; break;
    }
    (void)hm2;
    g_sOsd.scale = g_osdScale;
    SetWindowPos(g_osd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_osd, nullptr, FALSE);
}

static void ShowOsd() {
    const auto& list = BrightList();
    if (g_set.osdSel >= (int)list.size()) g_set.osdSel = -1;
    g_osdPct = TargetPercent();
    g_osdList = false;
    PlaceOsd();
    SetTimer(g_osd, TIMER_OSD, 2600, nullptr);
}

static void HideOsd() {
    if (!g_osd) return;
    FlushWmiPending();
    KillTimer(g_osd, TIMER_OSD);
    ShowWindow(g_osd, SW_HIDE);
    g_osdDrag = false;
    g_osdList = false;
    g_osdHoverItem = -1;
    g_osdChevronHover = false;
}

// ---------------------------------------------------------------------------
// 快捷键
// ---------------------------------------------------------------------------
static void ApplyHotkeys() {
    for (UINT i = 0; i < HK_COUNT; ++i) {
        UnregisterHotKey(g_hwnd, HK_ID_BASE + i);
        g_set.hk[i].ok = false;
    }
    if (g_captureHk >= 0) return;   // 录入时不占用
    for (UINT i = 0; i < HK_COUNT; ++i) {
        if (!g_set.hk[i].enabled || g_set.hk[i].vk == 0) continue;
        g_set.hk[i].ok = RegisterHotKey(g_hwnd, HK_ID_BASE + i, g_set.hk[i].mods | MOD_NOREPEAT, g_set.hk[i].vk) != FALSE;
    }
}

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------
static void AddTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"亮度");
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

static void RemoveTray() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
    if (g_nid.hIcon) {
        DestroyIcon(g_nid.hIcon);
        g_nid.hIcon = nullptr;
    }
}

static void ShowSettings(bool on) {
    g_settingsView = on;
    g_captureHk = -1;
    ApplyHotkeys();
    UpdateWindowSize();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"显示窗口");
    AppendMenuW(menu, MF_STRING, 2, L"亮度调节条");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"亮度 +5%");
    AppendMenuW(menu, MF_STRING, 4, L"亮度 -5%");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"退出");
    SetForegroundWindow(g_hwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case 1:
            ShowWindow(g_hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(g_hwnd);
            break;
        case 2: ShowOsd(); break;
        case 3: AdjustBy(5); break;
        case 4: AdjustBy(-5); break;
        case 5: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------
static void OnMainMouseMove(int x, int y, bool leave) {
    if (!g_settingsView && g_dragRow >= 0) {
        RequestSet(g_dragRow, ValueFromPos(g_dragRow, (float)x / g_scale));
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    float dx = (float)x / g_scale;
    float dy = (float)y / g_scale;
    int ctl = -1;

    auto in = [&](const D2D1_RECT_F& r) {
        return dx >= r.left && dx <= r.right && dy >= r.top && dy <= r.bottom;
    };

    if (!leave) {
        if (g_settingsView) {
            CfgLayout L = LayoutCfg();
            if (in(L.footerBtn)) ctl = 20;
            else if (in(L.autoToggle)) ctl = 3;
            else {
                for (int i = 0; i < 3; ++i)
                    if (in(L.hkToggle[i])) { ctl = i; break; }
                if (ctl < 0)
                    for (int i = 0; i < 3; ++i)
                        if (in(L.hkChip[i])) { ctl = 30 + i; break; }
                if (ctl < 0)
                    for (int i = 0; i < 6; ++i)
                        if (in(L.pos[i])) { ctl = 10 + i; break; }
            }
        } else {
            D2D1_RECT_F btn = {kWidth - 68.f, g_clientH - kFooterH + 3.f, kWidth - 12.f, g_clientH - 3.f};
            if (in(btn)) ctl = 20;
        }
    }

    int hitRow = -1;
    bool overSlider = false;
    if (!leave && !g_settingsView) {
        for (const auto& rb : g_rows) {
            if (dy >= rb.top && dy <= rb.top + kRowH) {
                hitRow = rb.index;
                overSlider = (dy >= rb.top + 37.f && dy <= rb.top + 69.f);
                break;
            }
        }
    }

    if (ctl != g_hoverCtl || hitRow != g_hoverRow || overSlider != g_hoverSlider) {
        g_hoverCtl = ctl;
        g_hoverRow = hitRow;
        g_hoverSlider = overSlider;
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    if (!leave && !g_trackingLeave) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, g_hwnd, 0};
        TrackMouseEvent(&tme);
        g_trackingLeave = true;
    }
}

static void OnMainClick(int x, int y) {
    float dx = (float)x / g_scale;
    float dy = (float)y / g_scale;
    CfgLayout L = LayoutCfg();
    auto in = [&](const D2D1_RECT_F& r) {
        return dx >= r.left && dx <= r.right && dy >= r.top && dy <= r.bottom;
    };

    if (in(L.footerBtn)) {
        ShowSettings(!g_settingsView);
        return;
    }

    if (g_settingsView) {
        for (int i = 0; i < 3; ++i) {
            if (in(L.hkToggle[i])) {
                g_set.hk[i].enabled = !g_set.hk[i].enabled;
                SaveSettings();
                ApplyHotkeys();
                InvalidateRect(g_hwnd, nullptr, FALSE);
                return;
            }
        }
        for (int i = 0; i < 3; ++i) {
            if (in(L.hkChip[i])) {
                g_captureHk = (g_captureHk == i) ? -1 : i;
                ApplyHotkeys();
                SetFocus(g_hwnd);
                InvalidateRect(g_hwnd, nullptr, FALSE);
                return;
            }
        }
        if (in(L.autoToggle)) {
            g_set.autostart = !g_set.autostart;
            SetAutoStart(g_set.autostart);
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        for (int i = 0; i < 6; ++i) {
            if (in(L.pos[i])) {
                g_set.osdPos = i;
                SaveSettings();
                ShowOsd();
                InvalidateRect(g_hwnd, nullptr, FALSE);
                return;
            }
        }
        return;
    }

    for (const auto& rb : g_rows) {
        if (dy >= rb.top && dy <= rb.top + kRowH) {
            if (!BrightList()[rb.index].controllable) return;
            g_dragRow = rb.index;
            SetCapture(g_hwnd);
            RequestSet(g_dragRow, ValueFromPos(g_dragRow, dx));
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
static void ApplyBackdrop(HWND hwnd, bool* micaFlag) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int backdrop = DWMSBT_MAINWINDOW;
    bool ok = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)));
    if (micaFlag) *micaFlag = ok;
}

static int OsdItemAt(float y) {
    if (!g_osdList) return -1;
    float top = kOsdH + 6.f;
    if (y < top) return -1;
    int idx = (int)((y - top) / kOsdRow);
    if (idx < 0 || idx >= OsdRowCount()) return -1;
    return idx;
}

static void OsdSelect(int row) {
    g_set.osdSel = OsdRowToSel(row);
    SaveSettings();
    g_osdList = false;
    g_osdPct = TargetPercent();
    PlaceOsd();
    SetTimer(g_osd, TIMER_OSD, 1800, nullptr);
    InvalidateRect(g_osd, nullptr, FALSE);
}

static LRESULT CALLBACK OsdProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            RenderOsd();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            ResizeSurface(g_sOsd, LOWORD(lp), HIWORD(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED: {
            g_osdScale = (float)HIWORD(wp) / 96.f;
            g_sOsd.scale = g_osdScale;
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, HWND_TOPMOST, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOACTIVATE);
            return 0;
        }

        case WM_MOUSEMOVE: {
            float x = (float)GET_X_LPARAM(lp) / g_osdScale;
            float y = (float)GET_Y_LPARAM(lp) / g_osdScale;

            if (g_osdDrag) {
                float t = Clamp01((x - OsdX1()) / (OsdX2() - OsdX1()));
                int pct = (int)lroundf(t * 100.f);
                if (GetTickCount() - g_osdLastWrite >= 30) {
                    SetPercent(pct);
                    g_osdLastWrite = GetTickCount();
                } else {
                    g_osdPct = pct;
                }
                SetTimer(hwnd, TIMER_OSD, 2600, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            bool chevHover = (x >= kOsdW - 44.f && x <= kOsdW - 6.f && y <= kOsdH);
            int item = OsdItemAt(y);
            if (chevHover != g_osdChevronHover || item != g_osdHoverItem) {
                g_osdChevronHover = chevHover;
                g_osdHoverItem = item;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetTimer(hwnd, TIMER_OSD, 2600, nullptr);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (!g_osdDrag) {
                g_osdChevronHover = false;
                g_osdHoverItem = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            float x = (float)GET_X_LPARAM(lp) / g_osdScale;
            float y = (float)GET_Y_LPARAM(lp) / g_osdScale;

            if (y > kOsdH) {
                int item = OsdItemAt(y);
                if (item >= 0) OsdSelect(item);
                return 0;
            }
            if (x >= kOsdW - 44.f && x <= kOsdW - 6.f) {
                g_osdList = !g_osdList;
                if (g_osdList) {
                    // 展开时补一条“全部显示器”，放在最前
                    g_osdHoverItem = -1;
                }
                PlaceOsd();
                SetTimer(hwnd, TIMER_OSD, 5200, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            g_osdDrag = true;
            SetCapture(hwnd);
            float t = Clamp01((x - OsdX1()) / (OsdX2() - OsdX1()));
            SetPercent((int)lroundf(t * 100.f));
            g_osdLastWrite = GetTickCount();
            SetTimer(hwnd, TIMER_OSD, 2600, nullptr);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_osdDrag) {
                float x = (float)GET_X_LPARAM(lp) / g_osdScale;
                float t = Clamp01((x - OsdX1()) / (OsdX2() - OsdX1()));
                SetPercent((int)lroundf(t * 100.f));
                FlushWmiPending();
                g_osdDrag = false;
                ReleaseCapture();
                SetTimer(hwnd, TIMER_OSD, 1800, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (!g_osdDrag) {
                int d = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 5 : -5;
                SetPercent(g_osdPct + d);
                SetTimer(hwnd, TIMER_OSD, 1800, nullptr);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) HideOsd();
            return 0;

        case WM_TIMER:
            if (wp == TIMER_OSD) {
                KillTimer(hwnd, TIMER_OSD);
                if (!g_osdDrag) HideOsd();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool HandleCapture(UINT vk) {
    if (g_captureHk < 0) return false;
    if (vk == VK_ESCAPE) {
        g_captureHk = -1;
        ApplyHotkeys();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return true;
    }
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS)
        return true;   // 等真正的键，吞掉

    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
    if (mods == 0) return true;   // 必须带修饰键

    int i = g_captureHk;
    g_set.hk[i].mods = mods;
    g_set.hk[i].vk = vk;
    g_set.hk[i].enabled = true;
    g_captureHk = -1;
    SaveSettings();
    ApplyHotkeys();
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return true;
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_CREATE:
            g_hwnd = hwnd;
            g_scale = (float)GetDpiForWindow(hwnd) / 96.f;
            ApplyBackdrop(hwnd, &g_mica);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            RenderMain();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            g_sMain.scale = g_scale;
            ResizeSurface(g_sMain, LOWORD(lp), HIWORD(lp));
            ComputeRows();
            if (g_dc) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED:
            g_scale = (float)HIWORD(wp) / 96.f;
            UpdateWindowSize();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEMOVE:
            OnMainMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false);
            return 0;

        case WM_MOUSELEAVE:
            g_trackingLeave = false;
            OnMainMouseMove(0, 0, true);
            return 0;

        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            OnMainClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_LBUTTONUP:
            if (g_dragRow >= 0) {
                FlushPending(g_dragRow);
                g_dragRow = -1;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            if (g_settingsView) return 0;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            float dy = (float)pt.y / g_scale;
            int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 5 : -5;
            for (const auto& rb : g_rows) {
                if (dy >= rb.top && dy <= rb.top + kRowH) {
                    const auto& d = BrightList()[rb.index];
                    if (!d.controllable) return 0;
                    int pct = BrightPercent(rb.index) + delta;
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    BrightSet(rb.index, d.min + (d.max - d.min) * pct / 100);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
            if (HandleCapture((UINT)wp)) return 0;
            if (wp == VK_ESCAPE) {
                if (g_settingsView) ShowSettings(false);
                else ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            if (wp == VK_F5) {
                BrightRefresh();
                UpdateWindowSize();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_TIMER:
            if (wp == TIMER_POLL) {
                if (IsWindowVisible(hwnd) && g_dragRow < 0) {
                    bool changed = false;
                    for (size_t i = 0; i < BrightList().size(); ++i) {
                        if (!BrightList()[i].controllable) continue;
                        int old = BrightList()[i].cur;
                        int v = 0;
                        if (BrightRead((int)i, v) && v != old) changed = true;
                    }
                    if (changed) InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (wp == TIMER_FLUSH) {
                KillTimer(hwnd, TIMER_FLUSH);
                bool any = false;
                for (int i = 0; i < 16; ++i) {
                    if (g_pending[i]) {
                        FlushPending(i);
                        any = true;
                    }
                }
                if (any) InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_HOTKEY: {
            UINT id = (UINT)wp - HK_ID_BASE;
            if (id == 0) AdjustBy(5);
            else if (id == 1) AdjustBy(-5);
            else if (id == 2) ShowOsd();
            return 0;
        }

        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP) {
                if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                else {
                    ShowWindow(hwnd, SW_SHOWNORMAL);
                    SetForegroundWindow(hwnd);
                }
            } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                ShowTrayMenu();
            }
            return 0;

        case WM_DISPLAYCHANGE:
            BrightRefresh();
            UpdateWindowSize();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL);
            KillTimer(hwnd, TIMER_FLUSH);
            for (UINT i = 0; i < HK_COUNT; ++i) UnregisterHotKey(hwnd, HK_ID_BASE + i);
            RemoveTray();
            BrightShutdown();
            if (g_ellipsis) g_ellipsis->Release();
            if (g_tfName) g_tfName->Release();
            if (g_tfPct) g_tfPct->Release();
            if (g_tfSmall) g_tfSmall->Release();
            if (g_tfTiny) g_tfTiny->Release();
            if (g_tfBig) g_tfBig->Release();
            if (g_writeFactory) g_writeFactory->Release();
            ReleaseGraphics();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\LumaBrightnessTool-Mutex");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND old = FindWindowW(kClassName, nullptr);
        if (old) {
            ShowWindow(old, SW_SHOWNORMAL);
            SetForegroundWindow(old);
        }
        return 0;
    }

    LoadSettings();
    g_set.autostart = IsAutoStart();
    BrightInit();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!RegisterClassExW(&wc)) return 1;

    WNDCLASSEXW wcOsd = {};
    wcOsd.cbSize = sizeof(wcOsd);
    wcOsd.lpfnWndProc = OsdProc;
    wcOsd.hInstance = inst;
    wcOsd.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcOsd.lpszClassName = kOsdClass;
    if (!RegisterClassExW(&wcOsd)) return 1;

    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int x = sw - (int)kWidth - 80;
    int y = GetSystemMetrics(SM_CYSCREEN) / 4;

    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClassName, L"亮度", style, x, y, (int)kWidth, 220, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) return 1;

    g_osd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kOsdClass,
                            L"", WS_POPUP, 0, 0, (int)kOsdW, (int)kOsdH, nullptr, nullptr, inst, nullptr);
    if (!g_osd) return 1;

    if (!CreateGraphics()) {
        MessageBoxW(hwnd, L"图形初始化失败", L"亮度", MB_ICONERROR);
        return 1;
    }

    UpdateWindowSize();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    ApplyHotkeys();
    AddTray();
    SetTimer(hwnd, TIMER_POLL, 5000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
