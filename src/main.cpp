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
static const UINT WM_TRAY = WM_APP + 1;
static const UINT TIMER_POLL = 1;
static const UINT TIMER_FLUSH = 2;

static const float kWidth = 404.f;
static const float kPad = 12.f;
static const float kRowH = 74.f;
static const float kGap = 8.f;
static const float kFooterH = 34.f;

static const D2D1_COLOR_F kText = {1.f, 1.f, 1.f, 0.94f};
static const D2D1_COLOR_F kTextDim = {1.f, 1.f, 1.f, 0.55f};
static const D2D1_COLOR_F kTextFaint = {1.f, 1.f, 1.f, 0.36f};
static const D2D1_COLOR_F kRowBg = {1.f, 1.f, 1.f, 0.045f};
static const D2D1_COLOR_F kRowHover = {1.f, 1.f, 1.f, 0.085f};
static const D2D1_COLOR_F kRowBorder = {1.f, 1.f, 1.f, 0.07f};
static const D2D1_COLOR_F kTrack = {1.f, 1.f, 1.f, 0.20f};
static const D2D1_COLOR_F kThumb = {1.f, 1.f, 1.f, 1.f};
static const D2D1_COLOR_F kBgFallback = {0.086f, 0.086f, 0.094f, 1.f};

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static HINSTANCE g_inst = nullptr;
static HWND g_hwnd = nullptr;
static float g_scale = 1.f;
static float g_clientH = 126.f;
static float g_ox = 0.f, g_oy = 0.f;   // 内容在客户区里的偏移（放大后居中）
static bool g_mica = false;
static D2D1_COLOR_F g_accent = {0.30f, 0.76f, 1.f, 1.f};

static ID3D11Device* g_d3d = nullptr;
static ID3D11DeviceContext* g_d3dCtx = nullptr;
static IDXGIFactory2* g_dxgiFactory = nullptr;
static IDXGISwapChain1* g_swapChain = nullptr;
static IDCompositionDevice* g_compDevice = nullptr;
static IDCompositionTarget* g_compTarget = nullptr;
static IDCompositionVisual* g_compVisual = nullptr;
static ID2D1Factory1* g_d2dFactory = nullptr;
static ID2D1Device* g_d2dDevice = nullptr;
static ID2D1DeviceContext* g_dc = nullptr;
static ID2D1Bitmap1* g_targetBitmap = nullptr;
static ID2D1SolidColorBrush* g_brush = nullptr;
static ID2D1StrokeStyle* g_roundStroke = nullptr;
static IDWriteFactory* g_writeFactory = nullptr;
static IDWriteTextFormat* g_tfName = nullptr;
static IDWriteTextFormat* g_tfPct = nullptr;
static IDWriteTextFormat* g_tfSmall = nullptr;
static IDWriteInlineObject* g_ellipsis = nullptr;

struct RowBox {
    float top = 0;
    int index = 0;
};
static std::vector<RowBox> g_rows;
static int g_hoverRow = -1;
static bool g_hoverSlider = false;
static int g_dragRow = -1;
static bool g_trackingLeave = false;

static int g_pendingVal[16] = {};
static bool g_pending[16] = {};
static DWORD g_lastWrite[16] = {};

static NOTIFYICONDATAW g_nid = {};
static bool g_trayAdded = false;

static const wchar_t* kHotkeyHint = L"滚轮调节  ·  Ctrl+Alt+↑↓ 全局";

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

static float CalcClientHeight(int count) {
    if (count <= 0) return 96.f;
    return kPad + count * kRowH + (count - 1) * kGap + kFooterH;
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
static void ReleaseGraphics() {
    if (g_brush) { g_brush->Release(); g_brush = nullptr; }
    if (g_targetBitmap) { g_targetBitmap->Release(); g_targetBitmap = nullptr; }
    if (g_dc) { g_dc->Release(); g_dc = nullptr; }
    if (g_d2dDevice) { g_d2dDevice->Release(); g_d2dDevice = nullptr; }
    if (g_d2dFactory) { g_d2dFactory->Release(); g_d2dFactory = nullptr; }
    if (g_roundStroke) { g_roundStroke->Release(); g_roundStroke = nullptr; }
    if (g_compTarget) { g_compTarget->Release(); g_compTarget = nullptr; }
    if (g_compVisual) { g_compVisual->Release(); g_compVisual = nullptr; }
    if (g_compDevice) { g_compDevice->Release(); g_compDevice = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_dxgiFactory) { g_dxgiFactory->Release(); g_dxgiFactory = nullptr; }
    if (g_d3dCtx) { g_d3dCtx->Release(); g_d3dCtx = nullptr; }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
}

static bool BindTargetBitmap() {
    if (!g_dc || !g_swapChain) return false;
    IDXGISurface* surface = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, kIID_IDXGISurface, (void**)&surface)) || !surface) return false;

    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    props.dpiX = 96.f * g_scale;
    props.dpiY = 96.f * g_scale;
    HRESULT hr = g_dc->CreateBitmapFromDxgiSurface(surface, &props, &g_targetBitmap);
    surface->Release();
    if (FAILED(hr)) return false;
    g_dc->SetTarget(g_targetBitmap);
    g_dc->SetDpi(96.f * g_scale, 96.f * g_scale);
    g_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
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

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    UINT w = (UINT)(rc.right > 1 ? rc.right : 1), h = (UINT)(rc.bottom > 1 ? rc.bottom : 1);

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = g_dxgiFactory->CreateSwapChainForComposition(g_d3d, &desc, nullptr, &g_swapChain);
    if (FAILED(hr) || !g_swapChain) {
        dxgiDevice->Release();
        return false;
    }

    if (FAILED(DCompositionCreateDevice(dxgiDevice, kIID_IDCompositionDevice, (void**)&g_compDevice)) || !g_compDevice) {
        dxgiDevice->Release();
        return false;
    }
    dxgiDevice->Release();

    g_compDevice->CreateTargetForHwnd(g_hwnd, TRUE, &g_compTarget);
    g_compDevice->CreateVisual(&g_compVisual);
    if (g_compVisual) g_compVisual->SetContent(g_swapChain);
    if (g_compTarget) g_compTarget->SetRoot(g_compVisual);
    g_compDevice->Commit();

    if (!BindTargetBitmap()) return false;

    D2D1_STROKE_STYLE_PROPERTIES ssp = {};
    ssp.startCap = D2D1_CAP_STYLE_ROUND;
    ssp.endCap = D2D1_CAP_STYLE_ROUND;
    g_d2dFactory->CreateStrokeStyle(ssp, nullptr, 0, &g_roundStroke);
    return true;
}

static void ResizeSwapChain(UINT w, UINT h) {
    if (!g_swapChain || w < 1 || h < 1) return;
    if (g_dc) g_dc->SetTarget(nullptr);
    if (g_targetBitmap) {
        g_targetBitmap->Release();
        g_targetBitmap = nullptr;
    }
    if (FAILED(g_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return;
    BindTargetBitmap();
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

// 按客户区尺寸反推窗口尺寸（含系统标题栏），固定大小、不可缩放
static void UpdateWindowSize() {
    int n = (int)BrightList().size();
    g_clientH = CalcClientHeight(n);
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

static void ComputeOrigin(int wPx, int hPx) {
    float cw = (float)wPx / g_scale;
    float ch = (float)hPx / g_scale;
    float needH = CalcClientHeight((int)BrightList().size());
    g_ox = cw > kWidth ? (cw - kWidth) * 0.5f : 0.f;
    g_oy = ch > needH ? (ch - needH) * 0.5f : 0.f;
}

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

    D2D1_RECT_F track = {x1, cy - 2.f, x2, cy + 2.f};
    FillRound(track, 2.f, kTrack);
    if (tx > x1 + 0.5f) {
        D2D1_RECT_F fill = {x1, cy - 2.f, tx, cy + 2.f};
        FillRound(fill, 2.f, g_accent);
    }

    float tr = 7.f;
    if (g_dragRow == rb.index) tr = 8.5f;
    else if (hover && g_hoverSlider) tr = 8.f;
    D2D1_ELLIPSE thumb = {D2D1_POINT_2F{tx, cy}, tr, tr};
    g_brush->SetColor(kThumb);
    g_dc->FillEllipse(&thumb, g_brush);
}

static void Render() {
    if (!g_dc || !g_hwnd || !g_targetBitmap) return;
    g_dc->BeginDraw();
    if (g_mica) {
        D2D1_COLOR_F clear = {0.f, 0.f, 0.f, 0.f};
        g_dc->Clear(&clear);
    } else {
        g_dc->Clear(&kBgFallback);
    }

    D2D1_MATRIX_3X2_F offset = {1.f, 0.f, 0.f, 1.f, g_ox, g_oy};
    g_dc->SetTransform(&offset);

    const auto& list = BrightList();
    bool anyCtl = false;
    for (const auto& d : list) {
        if (d.controllable) {
            anyCtl = true;
            break;
        }
    }
    if (list.empty()) {
        TextIn(L"未检测到显示器", g_tfName, D2D1_RECT_F{kPad, kPad + 20.f, kWidth - kPad, kPad + 50.f}, kTextDim);
    } else {
        for (const auto& rb : g_rows) DrawRow(rb, list[rb.index]);
    }

    const wchar_t* footer = (!list.empty() && !anyCtl) ? L"读不到亮度，按 F5 重新扫描" : kHotkeyHint;
    TextIn(footer, g_tfSmall, D2D1_RECT_F{20.f, g_clientH - kFooterH + 6.f, kWidth - 20.f, g_clientH - 8.f},
           kTextFaint);

    HRESULT hr = g_dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseGraphics();
        CreateGraphics();
        Render();
        return;
    }
    if (g_swapChain) g_swapChain->Present(1, 0);
}

// ---------------------------------------------------------------------------
// 调光（拖动时节流，避免 DDC 写入堆积）
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

static void AdjustAll(int delta) {
    const auto& list = BrightList();
    int shown = 0;
    for (size_t i = 0; i < list.size(); ++i) {
        if (!list[i].controllable) continue;
        int pct = BrightPercent((int)i) + delta;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        BrightSet((int)i, list[i].min + (list[i].max - list[i].min) * pct / 100);
        shown = pct;
    }
    if (g_trayAdded) {
        swprintf(g_nid.szTip, 128, L"亮度 %d%%", shown);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
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

static void ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"显示窗口");
    AppendMenuW(menu, MF_STRING, 2, L"亮度 +5%");
    AppendMenuW(menu, MF_STRING, 3, L"亮度 -5%");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"退出");
    SetForegroundWindow(g_hwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case 1:
            ShowWindow(g_hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(g_hwnd);
            break;
        case 2: AdjustAll(5); break;
        case 3: AdjustAll(-5); break;
        case 4: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------
static void OnMouseMove(int x, int y, bool leave) {
    if (g_dragRow >= 0) {
        RequestSet(g_dragRow, ValueFromPos(g_dragRow, (float)x / g_scale - g_ox));
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }

    float dy = (float)y / g_scale - g_oy;
    int hitRow = -1;
    bool overSlider = false;
    if (!leave) {
        for (const auto& rb : g_rows) {
            if (dy >= rb.top && dy <= rb.top + kRowH) {
                hitRow = rb.index;
                overSlider = (dy >= rb.top + 37.f && dy <= rb.top + 69.f);
                break;
            }
        }
    }
    if (hitRow != g_hoverRow || overSlider != g_hoverSlider) {
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

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
static void ApplyBackdrop() {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int backdrop = DWMSBT_MAINWINDOW;
    g_mica = SUCCEEDED(DwmSetWindowAttribute(g_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)));
}

static LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_CREATE:
            g_hwnd = hwnd;
            g_scale = (float)GetDpiForWindow(hwnd) / 96.f;
            ApplyBackdrop();
            UpdateWindowSize();
            if (!CreateGraphics()) {
                MessageBoxW(hwnd, L"图形初始化失败", L"亮度", MB_ICONERROR);
                return -1;
            }
            AddTray();
            SetTimer(hwnd, TIMER_POLL, 5000, nullptr);
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE:
            if (g_swapChain) ResizeSwapChain(LOWORD(lp), HIWORD(lp));
            ComputeOrigin(LOWORD(lp), HIWORD(lp));
            ComputeRows();
            if (g_dc) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED:
            g_scale = (float)HIWORD(wp) / 96.f;
            UpdateWindowSize();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false);
            return 0;

        case WM_MOUSELEAVE:
            g_trackingLeave = false;
            OnMouseMove(0, 0, true);
            return 0;

        case WM_LBUTTONDOWN: {
            float dx = (float)GET_X_LPARAM(lp) / g_scale - g_ox;
            float dy = (float)GET_Y_LPARAM(lp) / g_scale - g_oy;
            for (const auto& rb : g_rows) {
                if (dy >= rb.top && dy <= rb.top + kRowH) {
                    if (!BrightList()[rb.index].controllable) return 0;
                    g_dragRow = rb.index;
                    SetCapture(hwnd);
                    RequestSet(g_dragRow, ValueFromPos(g_dragRow, dx));
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (g_dragRow >= 0) {
                FlushPending(g_dragRow);
                g_dragRow = -1;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            float dy = (float)pt.y / g_scale - g_oy;
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

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                ShowWindow(hwnd, SW_HIDE);
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

        case WM_HOTKEY:
            if (wp == 1 || wp == 3) AdjustAll(5);
            else if (wp == 2 || wp == 4) AdjustAll(-5);
            return 0;

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
            UnregisterHotKey(hwnd, 1);
            UnregisterHotKey(hwnd, 2);
            UnregisterHotKey(hwnd, 3);
            UnregisterHotKey(hwnd, 4);
            RemoveTray();
            BrightShutdown();
            if (g_ellipsis) g_ellipsis->Release();
            if (g_tfName) g_tfName->Release();
            if (g_tfPct) g_tfPct->Release();
            if (g_tfSmall) g_tfSmall->Release();
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

    BrightInit();   // 建窗口之前先枚举，避免窗口状态干扰 DDC 句柄获取

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!RegisterClassExW(&wc)) return 1;

    // 系统原生标题栏：最小化/关闭按钮的高亮、按下、关闭变红都由 Windows 负责
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int x = sw - (int)kWidth - 80;
    int y = GetSystemMetrics(SM_CYSCREEN) / 4;

    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClassName, L"亮度", style, x, y, (int)kWidth, 150, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_UP);
    RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_DOWN);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    return 0;
}
