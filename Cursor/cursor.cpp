// Cursor Trail - Win32 + GDI+ (single file, no resource file needed)
//
// Visual Studio setup:
//   1. Create a new "Windows Desktop Application" (or "Empty Project") in C++.
//   2. Add this file as main.cpp (remove any auto-generated main/WinMain file).
//   3. Project Properties -> General -> Character Set: "Use Unicode Character Set".
//   4. Project Properties -> Linker -> System -> SubSystem: Windows (/SUBSYSTEM:WINDOWS).
//   5. Build & run (x64 or x86, Debug or Release - Release is much faster).
//
// The control window holds the menu bar. A transparent, click-through overlay
// window covers all monitors and draws the trail on top of everything.
// The control window's title bar shows live frame-time stats.
//
// Trail shapes:
//   - Continuous outline : the cursor's outline swept along the path into a
//                          gap-free trail (default; "heat buffer" renderer)
//   - Outline (ghosts)   : discrete ghost copies of the cursor's outline
//   - Filled (ghosts)    : discrete ghost copies of the cursor's silhouette
//   - Line               : a classic ribbon line following the pointer
//
// Continuous renderer: every pixel the cursor outline sweeps over stores the
// time it was last touched. Each frame only the new path is stamped (1.5 px
// steps), then active 32x32 tiles are colorized from a gradient lookup table
// by pixel age. Cost does not grow with speed or trail length, and the
// gradient (including animated rainbow) applies smoothly to the whole trail.
//
// Performance design:
//   * The overlay window is only as large as the trail's bounding box, and is
//     hidden when idle; only that region is cleared and presented each frame.
//   * Cursor ghosts are blitted in software (premultiplied alpha) from
//     pre-scaled alpha masks - no per-ghost GDI+ calls.
//   * Pointer sampling (~4 ms) is separate from rendering (one frame per
//     display refresh). Timing uses QueryPerformanceCounter.

#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <deque>
#include <vector>
#include <map>
#include <memory>
#include <cmath>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))


// ------------------------------------------------------------------ settings
enum Mode { MODE_SOLID = 0, MODE_GRADIENT = 1, MODE_RAINBOW = 2 };
enum Shape { SHAPE_OUTLINE = 0, SHAPE_FILLED = 1, SHAPE_LINE = 2, SHAPE_HEAT = 3 };

static const int MAX_COLORS = 10;

struct Settings {
    Mode     mode = MODE_GRADIENT;
    int      count = 4;                      // number of gradient colors (2..10)
    COLORREF colors[MAX_COLORS] = {
        RGB(0, 200, 255), RGB(120, 80, 255), RGB(255, 0, 200), RGB(255, 160, 0),
        RGB(255, 255, 0), RGB(0, 255, 0), RGB(255, 0, 0), RGB(0, 0, 255),
        RGB(255, 255, 255), RGB(0, 0, 0)
    };
    Shape    shape = SHAPE_HEAT;
    int      width = 12;                     // line width (Line shape)
    int      lifeMs = 600;                    // how long a trail point lives
    bool     enabled = true;
    bool     taper = true;                   // shrink toward the tail
    bool     glow = true;
};
static Settings g_set;

struct Preset { const wchar_t* name; Mode mode; int count; COLORREF cols[MAX_COLORS]; };
static const Preset kPresets[] = {
    { L"Neon",     MODE_GRADIENT, 3,{ RGB(0,255,200), RGB(120,80,255), RGB(255,0,220) } },
    { L"Fire",     MODE_GRADIENT, 5,{ RGB(255,255,200), RGB(255,230,60), RGB(255,140,0), RGB(220,20,0), RGB(80,0,0) } },
    { L"Ocean",    MODE_GRADIENT, 4,{ RGB(170,255,255), RGB(0,200,255), RGB(0,90,220), RGB(0,20,120) } },
    { L"Sunset",   MODE_GRADIENT, 5,{ RGB(255,220,90), RGB(255,130,60), RGB(240,60,120), RGB(150,0,190), RGB(40,0,110) } },
    { L"Forest",   MODE_GRADIENT, 4,{ RGB(210,255,100), RGB(100,220,60), RGB(0,140,70), RGB(0,50,40) } },
    { L"Spectrum (10 colors)", MODE_GRADIENT, 10,
        { RGB(255,0,0), RGB(255,128,0), RGB(255,230,0), RGB(150,255,0), RGB(0,220,60),
          RGB(0,230,230), RGB(0,130,255), RGB(40,40,255), RGB(150,0,255), RGB(255,0,200) } },
    { L"Rainbow (animated)", MODE_RAINBOW, 2,{ RGB(255,0,0), RGB(255,0,0) } },
    { L"White",    MODE_SOLID,    1,{ RGB(255,255,255) } },
};
static const int kWidths[] = { 4, 8, 12, 20, 30 };
static const int kLengths[] = { 300, 600, 1000, 1600 };
static const wchar_t* kLengthNames[] = { L"Short", L"Medium", L"Long", L"Very Long" };

// ------------------------------------------------------------------ menu ids
enum {
    ID_EXIT = 100,
    ID_ENABLED, ID_TAPER, ID_GLOW,
    ID_SHAPE_OUTLINE, ID_SHAPE_FILLED, ID_SHAPE_LINE, ID_SHAPE_HEAT,
    ID_MODE_SOLID, ID_MODE_GRADIENT, ID_MODE_RAINBOW,
    ID_NCOLORS_BASE = 150,   // + (n - 2), n = 2..10
    ID_EDITCOLOR_BASE = 170,   // + index 0..9
    ID_PRESET_BASE = 200,   // + index
    ID_WIDTH_BASE = 300,   // + index
    ID_LEN_BASE = 400    // + index
};

// ------------------------------------------------------------------ cursor shape cache
static const int   LEVELS = 8;      // pre-scaled sizes (taper)
static const float MIN_SCALE = 0.55f;  // smallest ghost scale (at the tail)
static const int   MAX_GHOSTS = 320;   // cap on ghosts per frame (spacing stretches only past this)
static const float GHOST_SPACING = 6.f; // px between ghosts along the path

struct Level {
    int   w = 0, h = 0;
    float hx = 0, hy = 0;              // hotspot inside this (padded, scaled) mask
    std::vector<BYTE> main;            // alpha mask of the shape
    std::vector<BYTE> glow;            // blurred alpha mask for the glow
};

struct CursorShape {
    int pad = 10;                      // padding around the cursor (room for glow)
    Level lv[LEVELS];                  // lv[0] = full size
};

struct TrailPt {
    float x, y;
    double t;                          // ms (QPC based)
    std::shared_ptr<CursorShape> shape;
};

struct Stamp {
    float        age;
    const Level* L;
    int          x0, y0;               // top-left in surface coordinates
};

// ------------------------------------------------------------------ globals
static HINSTANCE g_hInst;
static HWND      g_main, g_overlay;
static HMENU     g_menu;
static std::deque<TrailPt> g_pts;
static COLORREF  g_custom[16];

static std::map<HCURSOR, std::shared_ptr<CursorShape>> g_cache;
static std::shared_ptr<CursorShape> g_curShape;

// overlay backing surface
static int       g_vx, g_vy, g_vw, g_vh;
static HDC       g_memDC;
static HBITMAP   g_dib, g_oldBmp;
static BYTE* g_bits;
static size_t    g_stride;
static Bitmap* g_gdipBmp;
static Graphics* g_gfx;
static bool      g_overlayShown = false;
static RECT      g_prevBounds = { 0, 0, 0, 0 };   // area drawn last frame (surface coords)

// timing
static double    g_qpcFreq = 1.0;
static int       g_renderHz = 60;
static double    g_accClear, g_accDraw, g_accPresent, g_lastTitle;
static int       g_frames;

// continuous ("heat buffer") trail state
//   per pixel: bits 24..31 glow coverage, 16..23 outline coverage, 0..15 birth time (ms, mod 65536)
static const int   HEAT_TILE = 32;
static const float HEAT_SPACING = 1.5f;     // px between outline stamps along the path
static std::vector<DWORD> g_heat;
static std::vector<BYTE>  g_tileOn;         // active-tile flags
static int       g_tilesX, g_tilesY;
static bool      g_heatPosValid;
static float     g_heatX, g_heatY;
static double    g_heatT, g_heatLastT, g_heatLastRender;

static const UINT_PTR TIMER_SAMPLE = 1;
static const UINT_PTR TIMER_RENDER = 2;

// ------------------------------------------------------------------ helpers
static double NowMs() {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / g_qpcFreq;
}

static float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static int   ClampByte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static int   MinI(int a, int b) { return a < b ? a : b; }
static int   MaxI(int a, int b) { return a > b ? a : b; }
static inline unsigned Div255(unsigned v) { v += 128; return (v + (v >> 8)) >> 8; }

static bool  IsEmptyR(const RECT& r) { return r.left >= r.right || r.top >= r.bottom; }
static RECT  UnionR(const RECT& a, const RECT& b) {
    if (IsEmptyR(a)) return b;
    if (IsEmptyR(b)) return a;
    RECT r = { MinI(a.left, b.left), MinI(a.top, b.top), MaxI(a.right, b.right), MaxI(a.bottom, b.bottom) };
    return r;
}
static RECT ClampToSurface(RECT r) {
    r.left = MaxI(r.left, 0); r.top = MaxI(r.top, 0);
    r.right = MinI(r.right, g_vw); r.bottom = MinI(r.bottom, g_vh);
    return r;
}

static void HsvToRgb(float h, float s, float v, int& r, int& g, int& b) {
    h = fmodf(h, 360.f); if (h < 0) h += 360.f;
    float c = v * s, x = c * (1.f - fabsf(fmodf(h / 60.f, 2.f) - 1.f)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    r = (int)((rr + m) * 255.f + 0.5f);
    g = (int)((gg + m) * 255.f + 0.5f);
    b = (int)((bb + m) * 255.f + 0.5f);
}

// age: 0 = newest (head), 1 = oldest (tail). Returns RGB.
static void TrailColor(float age, double now, int& r, int& g, int& b) {
    age = Clamp01(age);
    switch (g_set.mode) {
    case MODE_SOLID:
        r = GetRValue(g_set.colors[0]); g = GetGValue(g_set.colors[0]); b = GetBValue(g_set.colors[0]);
        break;
    case MODE_GRADIENT: {
        int n = g_set.count < 2 ? 2 : g_set.count;
        float f = age * (float)(n - 1);
        int i = (int)f;
        float t = f - (float)i;
        if (i >= n - 1) { i = n - 2; t = 1.f; }
        COLORREF a = g_set.colors[i], c = g_set.colors[i + 1];
        r = (int)(GetRValue(a) + ((int)GetRValue(c) - (int)GetRValue(a)) * t + 0.5f);
        g = (int)(GetGValue(a) + ((int)GetGValue(c) - (int)GetGValue(a)) * t + 0.5f);
        b = (int)(GetBValue(a) + ((int)GetBValue(c) - (int)GetBValue(a)) * t + 0.5f);
        break;
    }
    case MODE_RAINBOW:
        HsvToRgb(age * 300.f + (float)fmod(now, 3600.0) * 0.1f, 1.f, 1.f, r, g, b);
        break;
    }
}

// ------------------------------------------------------------------ cursor silhouette extraction
static std::vector<BYTE> BoxBlur(const std::vector<BYTE>& src, int w, int h, int r) {
    std::vector<BYTE> dst(src.size(), 0);
    const int area = (2 * r + 1) * (2 * r + 1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int dy = -r; dy <= r; ++dy) {
                int yy = y + dy; if (yy < 0 || yy >= h) continue;
                for (int dx = -r; dx <= r; ++dx) {
                    int xx = x + dx; if (xx < 0 || xx >= w) continue;
                    sum += src[yy * w + xx];
                }
            }
            dst[y * w + x] = (BYTE)(sum / area);
        }
    return dst;
}

static std::vector<BYTE> Erode(const std::vector<BYTE>& m, int w, int h) {
    std::vector<BYTE> o(m.size(), 0);
    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
            bool all = true;
            for (int dy = -1; dy <= 1 && all; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (!m[(y + dy) * w + (x + dx)]) { all = false; break; }
            o[y * w + x] = all ? 255 : 0;
        }
    return o;
}

// Area-average downscale of an 8-bit mask.
static std::vector<BYTE> ScaleMask(const std::vector<BYTE>& src, int sw, int sh, int dw, int dh) {
    std::vector<BYTE> dst((size_t)dw * dh, 0);
    for (int y = 0; y < dh; ++y) {
        int y0 = (int)floorf(y * (float)sh / dh);
        int y1 = MinI(sh, MaxI(y0 + 1, (int)ceilf((y + 1) * (float)sh / dh)));
        for (int x = 0; x < dw; ++x) {
            int x0 = (int)floorf(x * (float)sw / dw);
            int x1 = MinI(sw, MaxI(x0 + 1, (int)ceilf((x + 1) * (float)sw / dw)));
            int sum = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) { sum += src[(size_t)yy * sw + xx]; ++n; }
            dst[(size_t)y * dw + x] = (BYTE)(n ? sum / n : 0);
        }
    }
    return dst;
}

// Renders the cursor on black and on white to recover per-pixel coverage
// (works for alpha cursors, classic mask cursors and XOR cursors alike).
static std::shared_ptr<CursorShape> BuildShape(HCURSOR hc, bool filled) {
    if (!hc) return nullptr;
    ICONINFO ii;
    if (!GetIconInfo(hc, &ii)) return nullptr;
    BITMAP bm = {};
    int w = 0, h = 0;
    if (ii.hbmColor) { GetObject(ii.hbmColor, sizeof(bm), &bm); w = bm.bmWidth; h = bm.bmHeight; }
    else if (ii.hbmMask) { GetObject(ii.hbmMask, sizeof(bm), &bm); w = bm.bmWidth; h = bm.bmHeight / 2; }
    int hx = (int)ii.xHotspot, hy = (int)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return nullptr;

    auto grab = [&](COLORREF bg, std::vector<BYTE>& out) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC sdc = GetDC(NULL);
        HDC dc = CreateCompatibleDC(sdc);
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        ReleaseDC(NULL, sdc);
        HGDIOBJ old = SelectObject(dc, dib);
        RECT rc = { 0, 0, w, h };
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        DrawIconEx(dc, 0, 0, hc, w, h, 0, NULL, DI_NORMAL);
        GdiFlush();
        out.assign((BYTE*)bits, (BYTE*)bits + (size_t)w * h * 4);
        SelectObject(dc, old);
        DeleteObject(dib);
        DeleteDC(dc);
        };

    std::vector<BYTE> onBlack, onWhite;
    grab(RGB(0, 0, 0), onBlack);
    grab(RGB(255, 255, 255), onWhite);

    auto cs = std::make_shared<CursorShape>();
    const int pad = cs->pad;
    const int bw = w + 2 * pad, bh = h + 2 * pad;

    // coverage (0..255), padded
    std::vector<BYTE> cov((size_t)bw * bh, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 4;
            int d = ((int)onWhite[i] - onBlack[i] + (int)onWhite[i + 1] - onBlack[i + 1] +
                (int)onWhite[i + 2] - onBlack[i + 2]) / 3;
            int a = d >= 0 ? 255 - d : 255;    // negative diff = XOR/inverted pixel
            cov[(size_t)(y + pad) * bw + (x + pad)] = (BYTE)ClampByte(a);
        }

    // binary silhouette -> 2px ring
    std::vector<BYTE> sil(cov.size());
    for (size_t i = 0; i < cov.size(); ++i) sil[i] = cov[i] >= 128 ? 255 : 0;
    std::vector<BYTE> e = Erode(Erode(sil, bw, bh), bw, bh);
    std::vector<BYTE> ring(cov.size());
    for (size_t i = 0; i < cov.size(); ++i) ring[i] = (sil[i] && !e[i]) ? 255 : 0;
    ring = BoxBlur(ring, bw, bh, 1);
    for (auto& v : ring) v = (BYTE)ClampByte((int)v * 2);

    std::vector<BYTE> mainA = filled ? cov : ring;

    // glow = double blur of the shape, normalized
    std::vector<BYTE> gl = BoxBlur(BoxBlur(mainA, bw, bh, 3), bw, bh, 3);
    int mx = 1;
    for (BYTE v : gl) if (v > mx) mx = v;
    for (auto& v : gl) v = (BYTE)ClampByte((int)v * 200 / mx);

    // level 0 = full size; the rest are pre-scaled for the taper effect
    for (int k = 0; k < LEVELS; ++k) {
        Level& L = cs->lv[k];
        if (k == 0) {
            L.w = bw; L.h = bh;
            L.hx = (float)(hx + pad); L.hy = (float)(hy + pad);
            L.main = mainA; L.glow = gl;
        }
        else {
            float s = 1.f - (1.f - MIN_SCALE) * (float)k / (float)(LEVELS - 1);
            L.w = MaxI(1, (int)(bw * s + 0.5f));
            L.h = MaxI(1, (int)(bh * s + 0.5f));
            L.hx = (float)(hx + pad) * L.w / bw;
            L.hy = (float)(hy + pad) * L.h / bh;
            L.main = ScaleMask(mainA, bw, bh, L.w, L.h);
            L.glow = ScaleMask(gl, bw, bh, L.w, L.h);
        }
    }
    return cs;
}

static std::shared_ptr<CursorShape> GetShape(HCURSOR hc) {
    auto it = g_cache.find(hc);
    if (it != g_cache.end()) return it->second;
    auto s = BuildShape(hc, g_set.shape == SHAPE_FILLED);
    g_cache[hc] = s;    // cache failures too, to avoid rebuilding every tick
    return s;
}

static void ResetShapes() {
    g_cache.clear();
    g_curShape.reset();
    for (auto& p : g_pts) p.shape.reset();
}

// ------------------------------------------------------------------ overlay surface
static void CreateSurface() {
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_vw;
    bi.bmiHeader.biHeight = -g_vh;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    g_memDC = CreateCompatibleDC(screen);
    void* bits = nullptr;
    g_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);
    g_bits = (BYTE*)bits;
    g_stride = (size_t)g_vw * 4;

    g_gdipBmp = new Bitmap(g_vw, g_vh, (INT)g_stride, PixelFormat32bppPARGB, g_bits);
    g_gfx = new Graphics(g_gdipBmp);
    g_gfx->SetSmoothingMode(SmoothingModeAntiAlias);

    g_prevBounds = RECT{ 0, 0, 0, 0 };
}

static void DestroySurface() {
    delete g_gfx;     g_gfx = nullptr;
    delete g_gdipBmp; g_gdipBmp = nullptr;
    if (g_memDC) { SelectObject(g_memDC, g_oldBmp); DeleteObject(g_dib); DeleteDC(g_memDC); }
    g_memDC = nullptr; g_dib = nullptr; g_bits = nullptr;
    std::vector<DWORD>().swap(g_heat);      // heat buffer is surface-sized
    std::vector<BYTE>().swap(g_tileOn);
    g_heatPosValid = false;
}

static void ClearRect(const RECT& r) {
    if (IsEmptyR(r)) return;
    size_t bytes = (size_t)(r.right - r.left) * 4;
    for (int y = r.top; y < r.bottom; ++y)
        memset(g_bits + (size_t)y * g_stride + (size_t)r.left * 4, 0, bytes);
}

// The overlay window is only as big as the trail's bounding box: it is moved and
// resized to `cur` every frame, and hidden when there is nothing to show. This
// avoids keeping a topmost layered window over the whole desktop.
static void PresentOverlay(const RECT& cur) {
    if (IsEmptyR(cur)) {
        if (g_overlayShown) { ShowWindow(g_overlay, SW_HIDE); g_overlayShown = false; }
        return;
    }
    POINT dst = { g_vx + cur.left, g_vy + cur.top };
    POINT src = { cur.left, cur.top };
    SIZE  sz = { cur.right - cur.left, cur.bottom - cur.top };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_overlay, NULL, &dst, &sz, g_memDC, &src, 0, &bf, ULW_ALPHA);
    if (!g_overlayShown) { ShowWindow(g_overlay, SW_SHOWNOACTIVATE); g_overlayShown = true; }
}

// ------------------------------------------------------------------ drawing
// Premultiplied src-over blit of a tinted alpha mask into the surface.
static void BlitMask(const std::vector<BYTE>& mask, int mw, int mh, int x0, int y0,
    int r, int g, int b, int af /* 0..256 */) {
    int xs = x0 < 0 ? -x0 : 0;
    int xe = (x0 + mw > g_vw) ? g_vw - x0 : mw;
    int ys = y0 < 0 ? -y0 : 0;
    int ye = (y0 + mh > g_vh) ? g_vh - y0 : mh;
    if (xs >= xe || ys >= ye) return;

    for (int y = ys; y < ye; ++y) {
        const BYTE* s = &mask[(size_t)y * mw];
        DWORD* d = (DWORD*)(g_bits + (size_t)(y0 + y) * g_stride) + x0;
        for (int x = xs; x < xe; ++x) {
            unsigned m = s[x];
            if (!m) continue;
            unsigned a = (m * (unsigned)af) >> 8;
            if (!a) continue;
            unsigned ia = 255 - a;
            DWORD dp = d[x];
            unsigned nb = Div255((unsigned)b * a + (dp & 0xFF) * ia);
            unsigned ng = Div255((unsigned)g * a + ((dp >> 8) & 0xFF) * ia);
            unsigned nr = Div255((unsigned)r * a + ((dp >> 16) & 0xFF) * ia);
            unsigned na = a + Div255((dp >> 24) * ia);
            d[x] = nb | (ng << 8) | (nr << 16) | (na << 24);
        }
    }
}

// Builds the ghost list (newest first) and its bounding box in surface coordinates.
static void BuildStamps(double now, std::vector<Stamp>& st, RECT& bounds) {
    st.clear();
    bounds = RECT{ 0, 0, 0, 0 };
    if (g_pts.empty()) return;
    const float life = (float)g_set.lifeMs;

    float total = 0.f;
    for (size_t i = 1; i < g_pts.size(); ++i) {
        float dx = g_pts[i].x - g_pts[i - 1].x, dy = g_pts[i].y - g_pts[i - 1].y;
        total += sqrtf(dx * dx + dy * dy);
    }
    float spacing = GHOST_SPACING;
    if (total / spacing > (float)MAX_GHOSTS) spacing = total / (float)MAX_GHOSTS;

    auto shapeOf = [&](const TrailPt& p) -> const CursorShape* {
        return p.shape ? p.shape.get() : g_curShape.get();
        };
    auto ageOf = [&](const TrailPt& p) { return Clamp01((float)(now - p.t) / life); };

    auto emit = [&](float x, float y, float age, const CursorShape* cs) {
        int k = g_set.taper ? (int)(age * (float)(LEVELS - 1) + 0.5f) : 0;
        if (k < 0) k = 0; if (k > LEVELS - 1) k = LEVELS - 1;
        const Level& L = cs->lv[k];
        int x0 = (int)floorf(x - (float)g_vx - L.hx + 0.5f);
        int y0 = (int)floorf(y - (float)g_vy - L.hy + 0.5f);
        st.push_back({ age, &L, x0, y0 });
        RECT r = { x0, y0, x0 + L.w, y0 + L.h };
        bounds = UnionR(bounds, r);
        };

    const TrailPt& newest = g_pts.back();
    if (const CursorShape* cs = shapeOf(newest)) emit(newest.x, newest.y, ageOf(newest), cs);

    float nextAt = spacing, travelled = 0.f;
    for (size_t i = g_pts.size() - 1; i >= 1; --i) {
        const TrailPt& a = g_pts[i];       // newer
        const TrailPt& b = g_pts[i - 1];   // older
        float dx = b.x - a.x, dy = b.y - a.y;
        float L = sqrtf(dx * dx + dy * dy);
        if (L > 0.001f) {
            while (nextAt - travelled <= L) {
                float f = (nextAt - travelled) / L;
                if (const CursorShape* cs = shapeOf(a)) {
                    float age = ageOf(a) + (ageOf(b) - ageOf(a)) * f;
                    emit(a.x + dx * f, a.y + dy * f, Clamp01(age), cs);
                }
                nextAt += spacing;
            }
            travelled += L;
        }
        if (i == 1) break;
    }
}

static void DrawStamps(double now, const std::vector<Stamp>& st) {
    // oldest first so the head is drawn on top
    for (size_t k = st.size(); k-- > 0; ) {
        const Stamp& s = st[k];
        int r, g, b;
        TrailColor(s.age, now, r, g, b);
        float alpha = 1.f - s.age;
        if (g_set.glow)
            BlitMask(s.L->glow, s.L->w, s.L->h, s.x0, s.y0, r, g, b, (int)(alpha * 0.9f * 256.f + 0.5f));
        BlitMask(s.L->main, s.L->w, s.L->h, s.x0, s.y0, r, g, b, (int)(alpha * 256.f + 0.5f));
    }
}

// ---- line (ribbon) trail
static void SegInfo(size_t i, double now, float& age, int& r, int& g, int& b, float& w) {
    const TrailPt& a = g_pts[i - 1];
    const TrailPt& c = g_pts[i];
    age = Clamp01((float)((now - a.t) + (now - c.t)) * 0.5f / (float)g_set.lifeMs);
    TrailColor(age, now, r, g, b);
    w = g_set.taper ? (1.f + (g_set.width - 1.f) * (1.f - age)) : (float)g_set.width;
}

static RECT LineBounds() {
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (const TrailPt& p : g_pts) {
        if (p.x < minx) minx = p.x; if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y; if (p.y > maxy) maxy = p.y;
    }
    int m = (int)ceilf(g_set.width * 1.8f) + 4;
    RECT r = { (int)floorf(minx) - g_vx - m, (int)floorf(miny) - g_vy - m,
               (int)ceilf(maxx) - g_vx + m,  (int)ceilf(maxy) - g_vy + m };
    return r;
}

static void DrawLineTrail(double now) {
    if (g_pts.size() < 2) return;

    if (g_set.glow) {
        static const float passW[] = { 3.2f, 2.2f, 1.5f };
        static const float passA[] = { 0.10f, 0.16f, 0.24f };
        for (int p = 0; p < 3; ++p)
            for (size_t i = 1; i < g_pts.size(); ++i) {
                float age, w; int r, g, b;
                SegInfo(i, now, age, r, g, b, w);
                Pen pen(Color((BYTE)((1.f - age) * passA[p] * 255.f), (BYTE)r, (BYTE)g, (BYTE)b), w * passW[p]);
                pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
                g_gfx->DrawLine(&pen, g_pts[i - 1].x - g_vx, g_pts[i - 1].y - g_vy, g_pts[i].x - g_vx, g_pts[i].y - g_vy);
            }
    }
    for (size_t i = 1; i < g_pts.size(); ++i) {
        float age, w; int r, g, b;
        SegInfo(i, now, age, r, g, b, w);
        Pen pen(Color((BYTE)((1.f - age) * 255.f), (BYTE)r, (BYTE)g, (BYTE)b), w);
        pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
        g_gfx->DrawLine(&pen, g_pts[i - 1].x - g_vx, g_pts[i - 1].y - g_vy, g_pts[i].x - g_vx, g_pts[i].y - g_vy);
    }
}

// ------------------------------------------------------------------ continuous (heat buffer) trail
static void EnsureHeat() {
    if (!g_heat.empty()) return;
    g_heat.assign((size_t)g_vw * g_vh, 0);
    g_tilesX = (g_vw + HEAT_TILE - 1) / HEAT_TILE;
    g_tilesY = (g_vh + HEAT_TILE - 1) / HEAT_TILE;
    g_tileOn.assign((size_t)g_tilesX * g_tilesY, 0);
}

static void HeatReset() {
    std::vector<DWORD>().swap(g_heat);
    std::vector<BYTE>().swap(g_tileOn);
    g_heatPosValid = false;
    g_heatLastT = NowMs();      // ignore any points sampled before now
}

// Stamps the cursor outline (+ glow) at (x, y), recording time t as the birth time.
static void HeatStamp(const CursorShape& cs, float x, float y, double t) {
    const Level& L = cs.lv[0];
    int x0 = (int)floorf(x - (float)g_vx - L.hx + 0.5f);
    int y0 = (int)floorf(y - (float)g_vy - L.hy + 0.5f);
    WORD birth = (WORD)((unsigned long long)t & 0xFFFF);
    if (!birth) birth = 1;

    int xs = x0 < 0 ? -x0 : 0;
    int xe = (x0 + L.w > g_vw) ? g_vw - x0 : L.w;
    int ys = y0 < 0 ? -y0 : 0;
    int ye = (y0 + L.h > g_vh) ? g_vh - y0 : L.h;
    if (xs >= xe || ys >= ye) return;

    // mark the tiles this stamp touches
    int tx0 = (x0 + xs) / HEAT_TILE, tx1 = (x0 + xe - 1) / HEAT_TILE;
    int ty0 = (y0 + ys) / HEAT_TILE, ty1 = (y0 + ye - 1) / HEAT_TILE;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
            g_tileOn[(size_t)ty * g_tilesX + tx] = 1;

    const unsigned life = (unsigned)g_set.lifeMs;
    for (int y = ys; y < ye; ++y) {
        const BYTE* m = &L.main[(size_t)y * L.w];
        const BYTE* g = &L.glow[(size_t)y * L.w];
        DWORD* row = &g_heat[(size_t)(y0 + y) * g_vw] + x0;
        for (int x = xs; x < xe; ++x) {
            unsigned mv = m[x], gv = g[x];
            if (!(mv | gv)) continue;
            DWORD w = row[x];
            unsigned oc = 0, og = 0;
            if (w) {
                unsigned oa = (WORD)(birth - (WORD)(w & 0xFFFF));
                if (oa < life) { oc = (w >> 16) & 0xFF; og = w >> 24; }   // old data still alive
            }
            unsigned nc = mv > oc ? mv : oc;
            unsigned ng = gv > og ? gv : og;
            row[x] = ((DWORD)ng << 24) | ((DWORD)nc << 16) | birth;
        }
    }
}

// Stamps the path travelled since the last frame. Returns true if anything was stamped.
static bool HeatIngest() {
    size_t n = g_pts.size(), start = n;
    while (start > 0 && g_pts[start - 1].t > g_heatLastT) --start;
    if (start == n) return false;

    bool any = false;
    for (size_t j = start; j < n; ++j) {
        const TrailPt& p = g_pts[j];
        const CursorShape* cs = p.shape ? p.shape.get() : g_curShape.get();
        if (!cs) { g_heatPosValid = false; continue; }
        if (g_heatPosValid && (p.t - g_heatT) > 100.0) g_heatPosValid = false;   // pointer was idle

        if (!g_heatPosValid) {
            HeatStamp(*cs, p.x, p.y, p.t);
            any = true;
        }
        else {
            float dx = p.x - g_heatX, dy = p.y - g_heatY;
            float len = sqrtf(dx * dx + dy * dy);
            int steps = (int)ceilf(len / HEAT_SPACING);
            if (steps > 300) steps = 300;
            for (int s = 1; s <= steps; ++s) {
                float f = (float)s / (float)steps;
                HeatStamp(*cs, g_heatX + dx * f, g_heatY + dy * f, g_heatT + (p.t - g_heatT) * f);
                any = true;
            }
        }
        g_heatX = p.x; g_heatY = p.y; g_heatT = p.t; g_heatPosValid = true;
    }
    g_heatLastT = g_pts[n - 1].t;
    return any;
}

static void HeatClearAll() {
    ClearRect(g_prevBounds);
    g_prevBounds = RECT{ 0, 0, 0, 0 };
    PresentOverlay(g_prevBounds);   // hides the overlay
    HeatReset();
}

static void RenderHeat(double now) {
    if (!g_set.enabled) {
        if (!g_heat.empty() || !IsEmptyR(g_prevBounds)) HeatClearAll();
        return;
    }
    if (g_heatLastRender > 0.0 && now - g_heatLastRender > 5000.0) HeatClearAll();   // e.g. after sleep
    g_heatLastRender = now;

    double t0 = NowMs();

    if (g_heat.empty()) {
        if (g_pts.empty() || g_pts.back().t <= g_heatLastT) return;   // nothing new, nothing allocated
        EnsureHeat();
    }
    bool stamped = HeatIngest();
    if (!stamped && IsEmptyR(g_prevBounds)) return;                   // idle

    double t1 = NowMs();

    // gradient lookup: index 0 = newest, 255 = oldest
    BYTE lr[256], lg[256], lb[256], lf[256];
    for (int i = 0; i < 256; ++i) {
        float age = (float)i / 255.f;
        int r, g, b;
        TrailColor(age, now, r, g, b);
        lr[i] = (BYTE)r; lg[i] = (BYTE)g; lb[i] = (BYTE)b;
        lf[i] = (BYTE)((1.f - age) * 255.f + 0.5f);
    }

    const WORD     nowB = (WORD)((unsigned long long)now & 0xFFFF);
    const unsigned life = (unsigned)g_set.lifeMs;
    const unsigned invLife = (255u << 16) / life;
    const bool     glow = g_set.glow;

    RECT bb = { 0, 0, 0, 0 };
    for (int ty = 0; ty < g_tilesY; ++ty)
        for (int tx = 0; tx < g_tilesX; ++tx) {
            BYTE& on = g_tileOn[(size_t)ty * g_tilesX + tx];
            if (!on) continue;
            int x0 = tx * HEAT_TILE, x1 = MinI(x0 + HEAT_TILE, g_vw);
            int y0 = ty * HEAT_TILE, y1 = MinI(y0 + HEAT_TILE, g_vh);
            bool any = false;

            for (int y = y0; y < y1; ++y) {
                const DWORD* hrow = &g_heat[(size_t)y * g_vw];
                DWORD* hw = &g_heat[(size_t)y * g_vw];
                DWORD* d = (DWORD*)(g_bits + (size_t)y * g_stride);
                for (int x = x0; x < x1; ++x) {
                    DWORD w = hrow[x];
                    if (!w) { d[x] = 0; continue; }
                    unsigned age = (WORD)(nowB - (WORD)(w & 0xFFFF));
                    if (age >= life) { hw[x] = 0; d[x] = 0; continue; }   // expired
                    any = true;

                    unsigned li = (age * invLife) >> 16;
                    if (li > 255) li = 255;
                    unsigned f = lf[li];
                    unsigned ar = Div255(((w >> 16) & 0xFF) * f);
                    unsigned a = ar;
                    if (glow) {
                        unsigned ag = Div255(Div255((w >> 24) * f) * 230u);   // glow at ~90%
                        a = ar + Div255(ag * (255 - ar));
                    }
                    if (!a) { d[x] = 0; continue; }
                    unsigned R = Div255(lr[li] * a), G = Div255(lg[li] * a), B = Div255(lb[li] * a);
                    d[x] = B | (G << 8) | (R << 16) | (a << 24);
                }
            }
            if (any) {
                RECT tr = { x0, y0, x1, y1 };
                bb = UnionR(bb, tr);
            }
            else {
                on = 0;     // tile is empty; its pixels were zeroed above
            }
        }
    double t2 = NowMs();

    PresentOverlay(bb);
    g_prevBounds = bb;
    double t3 = NowMs();

    g_accClear += t1 - t0;    // "clr"  = stamping the new path
    g_accDraw += t2 - t1;    // "draw" = colorizing active tiles
    g_accPresent += t3 - t2;
    ++g_frames;
}

// Call after changing the trail shape: clears the overlay and any heat state.
static void OnShapeChanged() {
    ClearRect(g_prevBounds);
    g_prevBounds = RECT{ 0, 0, 0, 0 };
    PresentOverlay(g_prevBounds);   // hides the overlay
    HeatReset();
    ResetShapes();
}

// ------------------------------------------------------------------ frame
static void RenderFrame() {
    double now = NowMs();

    // age out old points
    while (!g_pts.empty() && (now - g_pts.front().t) > (double)g_set.lifeMs) g_pts.pop_front();

    if (g_set.shape == SHAPE_HEAT) { RenderHeat(now); return; }

    static std::vector<Stamp> stamps;
    RECT cur = { 0, 0, 0, 0 };
    bool draw = g_set.enabled && !g_pts.empty();
    bool line = (g_set.shape == SHAPE_LINE);

    if (draw) {
        if (line) {
            if (g_pts.size() < 2) draw = false;
            else cur = LineBounds();
        }
        else {
            BuildStamps(now, stamps, cur);
            if (stamps.empty()) draw = false;
        }
    }
    cur = draw ? ClampToSurface(cur) : RECT{ 0, 0, 0, 0 };
    if (IsEmptyR(cur)) { draw = false; cur = RECT{ 0, 0, 0, 0 }; }   // entirely off-screen

    if (!draw && IsEmptyR(g_prevBounds)) return;   // nothing to do

    double t0 = NowMs();

    // clear previous + current bounds in the backing surface
    ClearRect(UnionR(g_prevBounds, cur));
    double t1 = NowMs();

    if (draw) {
        if (line) DrawLineTrail(now);
        else      DrawStamps(now, stamps);
    }
    double t2 = NowMs();

    PresentOverlay(cur);
    double t3 = NowMs();

    g_prevBounds = cur;

    g_accClear += t1 - t0;
    g_accDraw += t2 - t1;
    g_accPresent += t3 - t2;
    ++g_frames;
}

static void UpdateTitleStats() {
    double now = NowMs();
    if (now - g_lastTitle < 500.0) return;
    g_lastTitle = now;

    wchar_t buf[160];
    if (g_frames > 0) {
        double n = (double)g_frames;
        double c = g_accClear / n, d = g_accDraw / n, p = g_accPresent / n;
        swprintf_s(buf, L"Cursor Trail | %.2f ms (clr %.2f, draw %.2f, pres %.2f) | %d Hz",
            c + d + p, c, d, p, g_renderHz);
    }
    else {
        swprintf_s(buf, L"Cursor Trail | idle | %d Hz", g_renderHz);
    }
    SetWindowTextW(g_main, buf);
    g_accClear = g_accDraw = g_accPresent = 0; g_frames = 0;
}

// ------------------------------------------------------------------ sampling
static void SamplePointer() {
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    if (!g_set.enabled) { g_pts.clear(); return; }
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;

    std::shared_ptr<CursorShape> sh;
    if (g_set.shape != SHAPE_LINE) {
        sh = GetShape(ci.hCursor);
        if (sh) g_curShape = sh;
    }
    float px = (float)ci.ptScreenPos.x, py = (float)ci.ptScreenPos.y;
    double now = NowMs();

    if (g_pts.empty()) { g_pts.push_back({ px, py, now, sh }); return; }

    TrailPt& back = g_pts.back();
    if (back.x == px && back.y == py) return;

    // Keep the point list sparse (>= 2px apart) but always keep the head exact.
    if (g_pts.size() >= 2) {
        const TrailPt& prev = g_pts[g_pts.size() - 2];
        float dx = prev.x - px, dy = prev.y - py;
        if (dx * dx + dy * dy < 4.f) {
            back.x = px; back.y = py; back.t = now; back.shape = sh;
            return;
        }
    }
    g_pts.push_back({ px, py, now, sh });
}

static int QueryRefreshHz() {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    return 60;
}

static void StartTimers() {
    g_renderHz = QueryRefreshHz();
    UINT interval = (UINT)MaxI(1, 1000 / g_renderHz);   // floor: never slower than the display
    SetTimer(g_main, TIMER_SAMPLE, 4, NULL);
    SetTimer(g_main, TIMER_RENDER, interval, NULL);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProc(h, m, w, l);
}

// ------------------------------------------------------------------ menu
static void BuildMenu() {
    g_menu = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)file, L"&File");

    HMENU trail = CreatePopupMenu();
    AppendMenuW(trail, MF_STRING, ID_ENABLED, L"&Enabled");
    AppendMenuW(trail, MF_STRING, ID_GLOW, L"&Glow effect");
    AppendMenuW(trail, MF_STRING, ID_TAPER, L"&Taper toward tail (ghost shapes)");
    AppendMenuW(trail, MF_SEPARATOR, 0, NULL);
    AppendMenuW(trail, MF_STRING, ID_SHAPE_HEAT, L"Shape: Cursor outline - &continuous");
    AppendMenuW(trail, MF_STRING, ID_SHAPE_OUTLINE, L"Shape: Cursor &outline (ghosts)");
    AppendMenuW(trail, MF_STRING, ID_SHAPE_FILLED, L"Shape: Cursor &filled (ghosts)");
    AppendMenuW(trail, MF_STRING, ID_SHAPE_LINE, L"Shape: &Line");
    AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)trail, L"&Trail");

    HMENU color = CreatePopupMenu();
    AppendMenuW(color, MF_STRING, ID_MODE_SOLID, L"&Solid color");
    AppendMenuW(color, MF_STRING, ID_MODE_GRADIENT, L"Multi-color &gradient");
    AppendMenuW(color, MF_STRING, ID_MODE_RAINBOW, L"&Rainbow (animated)");
    AppendMenuW(color, MF_SEPARATOR, 0, NULL);

    HMENU ncol = CreatePopupMenu();
    for (int n = 2; n <= MAX_COLORS; ++n) {
        wchar_t buf[32]; wsprintfW(buf, L"%d colors", n);
        AppendMenuW(ncol, MF_STRING, ID_NCOLORS_BASE + (n - 2), buf);
    }
    AppendMenuW(color, MF_POPUP, (UINT_PTR)ncol, L"Gradient color &count");

    HMENU edit = CreatePopupMenu();
    for (int i = 0; i < MAX_COLORS; ++i) {
        wchar_t buf[48]; wsprintfW(buf, L"Color %d%s...", i + 1, i == 0 ? L" (head / solid)" : L"");
        AppendMenuW(edit, MF_STRING, ID_EDITCOLOR_BASE + i, buf);
    }
    AppendMenuW(color, MF_POPUP, (UINT_PTR)edit, L"&Edit color");
    AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)color, L"&Color");

    HMENU presets = CreatePopupMenu();
    for (int i = 0; i < COUNT(kPresets); ++i)
        AppendMenuW(presets, MF_STRING, ID_PRESET_BASE + i, kPresets[i].name);
    AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)presets, L"&Presets");

    HMENU width = CreatePopupMenu();
    for (int i = 0; i < COUNT(kWidths); ++i) {
        wchar_t buf[32]; wsprintfW(buf, L"%d px", kWidths[i]);
        AppendMenuW(width, MF_STRING, ID_WIDTH_BASE + i, buf);
    }
    AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)width, L"&Width (Line)");

    HMENU len = CreatePopupMenu();
    for (int i = 0; i < COUNT(kLengths); ++i)
        AppendMenuW(len, MF_STRING, ID_LEN_BASE + i, kLengthNames[i]);
    AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)len, L"&Length");
}

static void UpdateMenuChecks() {
    CheckMenuItem(g_menu, ID_ENABLED, MF_BYCOMMAND | (g_set.enabled ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, ID_TAPER, MF_BYCOMMAND | (g_set.taper ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, ID_GLOW, MF_BYCOMMAND | (g_set.glow ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(g_menu, ID_SHAPE_OUTLINE, ID_SHAPE_HEAT, ID_SHAPE_OUTLINE + (int)g_set.shape, MF_BYCOMMAND);
    CheckMenuRadioItem(g_menu, ID_MODE_SOLID, ID_MODE_RAINBOW, ID_MODE_SOLID + (int)g_set.mode, MF_BYCOMMAND);
    CheckMenuRadioItem(g_menu, ID_NCOLORS_BASE, ID_NCOLORS_BASE + MAX_COLORS - 2,
        ID_NCOLORS_BASE + (g_set.count - 2), MF_BYCOMMAND);

    int wSel = -1;
    for (int i = 0; i < COUNT(kWidths); ++i) if (kWidths[i] == g_set.width) wSel = i;
    if (wSel >= 0) CheckMenuRadioItem(g_menu, ID_WIDTH_BASE, ID_WIDTH_BASE + COUNT(kWidths) - 1, ID_WIDTH_BASE + wSel, MF_BYCOMMAND);
    int lSel = -1;
    for (int i = 0; i < COUNT(kLengths); ++i) if (kLengths[i] == g_set.lifeMs) lSel = i;
    if (lSel >= 0) CheckMenuRadioItem(g_menu, ID_LEN_BASE, ID_LEN_BASE + COUNT(kLengths) - 1, ID_LEN_BASE + lSel, MF_BYCOMMAND);

    // which color slots are in use
    for (int i = 0; i < MAX_COLORS; ++i) {
        bool on = false;
        if (g_set.mode == MODE_SOLID)    on = (i == 0);
        if (g_set.mode == MODE_GRADIENT) on = (i < g_set.count);
        EnableMenuItem(g_menu, ID_EDITCOLOR_BASE + i, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
    }
    for (int n = 2; n <= MAX_COLORS; ++n)
        EnableMenuItem(g_menu, ID_NCOLORS_BASE + (n - 2), MF_BYCOMMAND | (g_set.mode == MODE_GRADIENT ? MF_ENABLED : MF_GRAYED));
    DrawMenuBar(g_main);
}

static bool PickColor(COLORREF& c) {
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_main;
    cc.rgbResult = c;
    cc.lpCustColors = g_custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) { c = cc.rgbResult; return true; }
    return false;
}

// ------------------------------------------------------------------ main window
static void PaintPreview(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);

    HBRUSH bg = CreateSolidBrush(RGB(30, 30, 34));
    FillRect(dc, &rc, bg); DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 220, 220));
    RECT t = { 12, 8, rc.right - 12, 30 };
    DrawTextW(dc, L"Cursor Trail is running. Use the menu bar to customize.", -1, &t, DT_LEFT | DT_SINGLELINE);

    // gradient preview: left = head (newest), right = tail (oldest)
    int x0 = 12, x1 = rc.right - 12, y0 = 40, y1 = 66;
    int span = x1 - x0;
    for (int x = 0; x < span; x += 2) {
        float age = (float)x / (float)span;
        int r, g, b; TrailColor(age, 0.0, r, g, b);
        HBRUSH br = CreateSolidBrush(RGB(r, g, b));
        RECT s = { x0 + x, y0, x0 + x + 2, y1 };
        FillRect(dc, &s, br); DeleteObject(br);
    }

    // swatches for the colors in use
    if (g_set.mode != MODE_RAINBOW) {
        int n = (g_set.mode == MODE_SOLID) ? 1 : g_set.count;
        for (int i = 0; i < n; ++i) {
            HBRUSH br = CreateSolidBrush(g_set.colors[i]);
            RECT s = { x0 + i * 26, 72, x0 + i * 26 + 22, 90 };
            FillRect(dc, &s, br); DeleteObject(br);
            FrameRect(dc, &s, (HBRUSH)GetStockObject(GRAY_BRUSH));
        }
    }
    RECT lab = { 12, 96, rc.right - 12, 118 };
    SetTextColor(dc, RGB(150, 150, 150));
    DrawTextW(dc, L"newest  ->  oldest   (frame stats are in the title bar)", -1, &lab, DT_LEFT | DT_SINGLELINE);
    EndPaint(h, &ps);
}

static void SettingsChanged() {
    UpdateMenuChecks();
    InvalidateRect(g_main, NULL, FALSE);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        if (w == TIMER_SAMPLE) {
            SamplePointer();
        }
        else if (w == TIMER_RENDER) {
            RenderFrame();
            UpdateTitleStats();
        }
        return 0;

    case WM_DISPLAYCHANGE: {
        ShowWindow(g_overlay, SW_HIDE);
        g_overlayShown = false;
        DestroySurface();
        CreateSurface();
        KillTimer(h, TIMER_SAMPLE);
        KillTimer(h, TIMER_RENDER);
        StartTimers();
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_EXIT) { DestroyWindow(h); return 0; }
        if (id == ID_ENABLED)       g_set.enabled = !g_set.enabled;
        else if (id == ID_TAPER)    g_set.taper = !g_set.taper;
        else if (id == ID_GLOW)     g_set.glow = !g_set.glow;
        else if (id >= ID_SHAPE_OUTLINE && id <= ID_SHAPE_HEAT) {
            Shape ns = (Shape)(id - ID_SHAPE_OUTLINE);
            if (ns != g_set.shape) { g_set.shape = ns; OnShapeChanged(); }
        }
        else if (id == ID_MODE_SOLID)    g_set.mode = MODE_SOLID;
        else if (id == ID_MODE_GRADIENT) g_set.mode = MODE_GRADIENT;
        else if (id == ID_MODE_RAINBOW)  g_set.mode = MODE_RAINBOW;
        else if (id >= ID_NCOLORS_BASE && id <= ID_NCOLORS_BASE + MAX_COLORS - 2)
            g_set.count = (id - ID_NCOLORS_BASE) + 2;
        else if (id >= ID_EDITCOLOR_BASE && id < ID_EDITCOLOR_BASE + MAX_COLORS)
            PickColor(g_set.colors[id - ID_EDITCOLOR_BASE]);
        else if (id >= ID_PRESET_BASE && id < ID_PRESET_BASE + COUNT(kPresets)) {
            const Preset& p = kPresets[id - ID_PRESET_BASE];
            g_set.mode = p.mode;
            g_set.count = p.count < 2 ? 2 : p.count;
            for (int i = 0; i < MAX_COLORS; ++i) g_set.colors[i] = p.cols[i];
            if (p.count < 2 && p.mode == MODE_SOLID) g_set.colors[1] = p.cols[0];
        }
        else if (id >= ID_WIDTH_BASE && id < ID_WIDTH_BASE + COUNT(kWidths))
            g_set.width = kWidths[id - ID_WIDTH_BASE];
        else if (id >= ID_LEN_BASE && id < ID_LEN_BASE + COUNT(kLengths))
            g_set.lifeMs = kLengths[id - ID_LEN_BASE];
        else break;
        SettingsChanged();
        return 0;
    }

    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintPreview(h); return 0;

    case WM_DESTROY:
        KillTimer(h, TIMER_SAMPLE);
        KillTimer(h, TIMER_RENDER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ------------------------------------------------------------------ entry point
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    g_hInst = hInst;
    SetProcessDPIAware();   // so cursor coords match the overlay pixels

    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_qpcFreq = (double)f.QuadPart;
    timeBeginPeriod(1);     // finer timer resolution for WM_TIMER

    GdiplusStartupInput gsi;
    ULONG_PTR gdipToken;
    GdiplusStartup(&gdipToken, &gsi, NULL);

    // main (control) window
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"CursorTrailMain";
    RegisterClassW(&wc);

    // overlay window
    WNDCLASSW oc = {};
    oc.lpfnWndProc = OverlayProc;
    oc.hInstance = hInst;
    oc.lpszClassName = L"CursorTrailOverlay";
    RegisterClassW(&oc);

    BuildMenu();

    RECT r = { 0, 0, 460, 126 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, TRUE);
    g_main = CreateWindowW(L"CursorTrailMain", L"Cursor Trail", style,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        NULL, g_menu, hInst, NULL);

    CreateSurface();
    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"CursorTrailOverlay", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, hInst, NULL);
    // The overlay stays hidden until the first frame with something to draw.

    UpdateMenuChecks();
    ShowWindow(g_main, nShow);
    UpdateWindow(g_main);
    StartTimers();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // release GDI+ objects before shutting GDI+ down
    g_pts.clear();
    g_cache.clear();
    g_curShape.reset();
    DestroyWindow(g_overlay);
    DestroySurface();
    GdiplusShutdown(gdipToken);
    timeEndPeriod(1);
    return (int)msg.wParam;
}
