// 生成 app.ico（多尺寸，32bpp + alpha）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <stdio.h>
#include <vector>

using namespace Gdiplus;

#pragma comment(lib, "gdiplus.lib")

static const int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 128};

static void DrawIcon(Graphics& g, int s) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));

    float pad = s * 0.02f;
    float r = s * 0.235f;
    RectF rc(pad, pad, (float)s - pad * 2, (float)s - pad * 2);

    GraphicsPath path;
    float d = r * 2.f;
    path.AddArc(rc.X, rc.Y, d, d, 180, 90);
    path.AddArc(rc.GetRight() - d, rc.Y, d, d, 270, 90);
    path.AddArc(rc.GetRight() - d, rc.GetBottom() - d, d, d, 0, 90);
    path.AddArc(rc.X, rc.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();

    LinearGradientBrush bg(PointF(0, 0), PointF(0, (REAL)s), Color(255, 84, 190, 255), Color(255, 32, 108, 224));
    g.FillPath(&bg, &path);

    // 太阳
    float cx = s * 0.5f, cy = s * 0.5f;
    float rays = s * 0.335f;
    float inner = s * 0.225f;
    float dot = s * 0.115f;

    Pen pen(Color(255, 255, 255, 255), s * 0.075f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    for (int i = 0; i < 8; ++i) {
        double a = i * 3.14159265358979 / 4.0;
        REAL dx = (REAL)cos(a), dy = (REAL)sin(a);
        g.DrawLine(&pen, PointF(cx + dx * inner, cy + dy * inner), PointF(cx + dx * rays, cy + dy * rays));
    }
    SolidBrush white(Color(255, 255, 255, 255));
    g.FillEllipse(&white, cx - dot, cy - dot, dot * 2, dot * 2);
}

static bool MakeBitmap(int s, std::vector<BYTE>& xorOut, std::vector<BYTE>& andOut) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP hbm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        DeleteDC(hdc);
        return false;
    }
    HGDIOBJ old = SelectObject(hdc, hbm);

    {
        Graphics g(hdc);
        DrawIcon(g, s);
    }

    const int rowBytes = s * 4;
    xorOut.assign((size_t)rowBytes * s, 0);
    BYTE* src = (BYTE*)bits;
    // top-down -> bottom-up
    for (int y = 0; y < s; ++y) {
        memcpy(&xorOut[(size_t)(s - 1 - y) * rowBytes], src + (size_t)y * rowBytes, rowBytes);
    }

    int maskRow = ((s + 31) / 32) * 4;
    andOut.assign((size_t)maskRow * s, 0);

    SelectObject(hdc, old);
    DeleteObject(hbm);
    DeleteDC(hdc);
    return true;
}

static void Put32(FILE* f, DWORD v) { fwrite(&v, 4, 1, f); }
static void Put16(FILE* f, WORD v) { fwrite(&v, 2, 1, f); }

int main() {
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) return 1;

    struct Item {
        int size;
        std::vector<BYTE> xorB, andB;
    };
    std::vector<Item> items;
    for (int s : kSizes) {
        Item it;
        it.size = s;
        if (!MakeBitmap(s, it.xorB, it.andB)) return 1;
        items.push_back(it);
    }

    FILE* f = fopen("app.ico", "wb");
    if (!f) return 1;
    Put16(f, 0);
    Put16(f, 1);
    Put16(f, (WORD)items.size());

    DWORD offset = 6 + 16 * (DWORD)items.size();
    for (auto& it : items) {
        BYTE w = (BYTE)(it.size >= 256 ? 0 : it.size);
        BYTE h = w;
        fwrite(&w, 1, 1, f);
        fwrite(&h, 1, 1, f);
        fputc(0, f);
        fputc(0, f);
        Put16(f, 1);
        Put16(f, 32);
        DWORD bytes = 40 + (DWORD)it.xorB.size() + (DWORD)it.andB.size();
        Put32(f, bytes);
        Put32(f, offset);
        offset += bytes;
    }

    for (auto& it : items) {
        Put32(f, 40);
        Put32(f, (DWORD)it.size);
        Put32(f, (DWORD)it.size * 2);
        Put16(f, 1);
        Put16(f, 32);
        Put32(f, 0);  // biCompression
        Put32(f, 0);  // biSizeImage
        Put32(f, 0);  // biXPelsPerMeter
        Put32(f, 0);  // biYPelsPerMeter
        Put32(f, 0);  // biClrUsed
        Put32(f, 0);  // biClrImportant
        fwrite(it.xorB.data(), 1, it.xorB.size(), f);
        fwrite(it.andB.data(), 1, it.andB.size(), f);
    }
    fclose(f);
    GdiplusShutdown(token);
    printf("app.ico written\n");
    return 0;
}
