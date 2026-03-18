#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ── Config ──────────────────────────────────────────────────────────────
static int   g_diameter   = 300;
static int   g_border     = 4;
static float g_zoom       = 2.0f;
static const float ZOOM_MIN = 1.0f;
static const float ZOOM_MAX = 8.0f;

// ── Refresh ─────────────────────────────────────────────────────────────
static const UINT FPS_INTERVAL = 16;    // ~60 FPS

// ── Globals ─────────────────────────────────────────────────────────────
static HWND    g_hWnd       = nullptr;
static int     g_winX, g_winY;
static bool    g_visible    = true;

// Mouse hook
static HHOOK   g_mouseHook  = nullptr;
static bool    g_dragging   = false;
static bool    g_dragConfirmed = false;
static bool    g_dragIsLeft = false;
static POINT   g_dragStart  = {};
static int     g_dragWinX, g_dragWinY;
static const int DRAG_THRESHOLD = 5;

// Zoom tracking
static float   g_lastRenderedZoom = 0;

// Capture mode: false = WDA_EXCLUDEFROMCAPTURE, true = per-window capture
static bool    g_useFallback = false;

// Cached GDI
static HDC     g_hScreenDC  = nullptr;
static HDC     g_hMemDC     = nullptr;
static HBITMAP g_hDIB       = nullptr;
static void*   g_pBits      = nullptr;
static HDC     g_hCapDC     = nullptr;  // zoomed capture (d x d)
static HBITMAP g_hCapBmp    = nullptr;
static HDC     g_hCompDC    = nullptr;  // 1:1 composition buffer (d x d max)
static HBITMAP g_hCompBmp   = nullptr;
static HDC     g_hBorderDC  = nullptr;
static HBITMAP g_hBorderBmp = nullptr;
static void*   g_pBorderBits = nullptr;
static bool    g_borderDirty = true;

// Circle mask
static BYTE*   g_circleMask = nullptr;
struct RowSpan { int x0, x1; };
static RowSpan* g_rowSpans  = nullptr;

// Hotkey / timer IDs
enum { HK_TOGGLE = 1, HK_ZOOM_IN, HK_ZOOM_OUT, HK_QUIT };
enum { TIMER_REFRESH = 1 };

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// ── Forward declarations ────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelMouseProc(int, WPARAM, LPARAM);
void LoadConfig();
void CreateCachedResources();
void DestroyCachedResources();
void RenderBorderCache();
void BuildCircleMask();
void CaptureAndRender();
void CaptureByWindowEnum(int cx, int cy, int srcSize);
bool IsPointInLens(int px, int py);

// ── Config file loading ──────────────────────────────────────────────────
void LoadConfig() {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(iniPath, L'\\');
    if (lastSlash) wcscpy(lastSlash + 1, L"config.ini");
    else wcscpy(iniPath, L"config.ini");

    int d = GetPrivateProfileIntW(L"magnifier", L"diameter", g_diameter, iniPath);
    if (d >= 100 && d <= 1000) g_diameter = d;

    int b = GetPrivateProfileIntW(L"magnifier", L"border", g_border, iniPath);
    if (b >= 1 && b <= 20) g_border = b;

    int z = GetPrivateProfileIntW(L"magnifier", L"zoom_x100", (int)(g_zoom * 100), iniPath);
    float zf = z / 100.0f;
    if (zf >= ZOOM_MIN && zf <= ZOOM_MAX) g_zoom = zf;
}

// ── Per-window screen capture (fallback for Win10 < 2004) ───────────────
// Reconstructs the screen image by compositing individual window DCs,
// skipping our own lens window. No flicker, no offset, no API version req.

struct WndEnumData {
    HWND excludeWnd;
    RECT srcRect;
    HWND windows[128];
    int count;
};

static BOOL CALLBACK CollectWindows(HWND hwnd, LPARAM lP) {
    auto* data = (WndEnumData*)lP;
    if (hwnd == data->excludeWnd) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;

    // Skip cloaked windows (hidden UWP apps, virtual desktops)
    DWORD cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;

    RECT wr;
    GetWindowRect(hwnd, &wr);

    RECT inter;
    if (IntersectRect(&inter, &wr, &data->srcRect)) {
        if (data->count < 128)
            data->windows[data->count++] = hwnd;
    }
    return TRUE;
}

void CaptureByWindowEnum(int cx, int cy, int srcSize) {
    int d = g_diameter;
    RECT srcRect = {
        cx - srcSize / 2,
        cy - srcSize / 2,
        cx - srcSize / 2 + srcSize,
        cy - srcSize / 2 + srcSize
    };

    // 1. Desktop wallpaper + icons (Progman is always at the bottom)
    HWND hProgman = FindWindow(L"Progman", nullptr);
    if (hProgman) {
        HDC hDeskDC = GetWindowDC(hProgman);
        if (hDeskDC) {
            BitBlt(g_hCompDC, 0, 0, srcSize, srcSize,
                   hDeskDC, srcRect.left, srcRect.top, SRCCOPY);
            ReleaseDC(hProgman, hDeskDC);
        }
    } else {
        RECT r = { 0, 0, srcSize, srcSize };
        FillRect(g_hCompDC, &r, (HBRUSH)(COLOR_DESKTOP + 1));
    }

    // 2. Collect visible windows overlapping source rect (top→bottom Z-order)
    WndEnumData data = {};
    data.excludeWnd = g_hWnd;
    data.srcRect = srcRect;
    EnumWindows(CollectWindows, (LPARAM)&data);

    // 3. Composite bottom→top (reverse enumeration order)
    for (int i = data.count - 1; i >= 0; i--) {
        HWND hwnd = data.windows[i];
        RECT wr;
        GetWindowRect(hwnd, &wr);

        RECT inter;
        if (!IntersectRect(&inter, &wr, &srcRect)) continue;

        int wxSrc = inter.left - wr.left;
        int wySrc = inter.top  - wr.top;
        int w     = inter.right  - inter.left;
        int h     = inter.bottom - inter.top;
        int dstX  = inter.left - srcRect.left;
        int dstY  = inter.top  - srcRect.top;

        // PrintWindow with PW_RENDERFULLCONTENT captures DX/GL content
        // that returns black via GetWindowDC/BitBlt on Win10.
        int wndW = wr.right - wr.left;
        int wndH = wr.bottom - wr.top;
        HDC hTmpDC = CreateCompatibleDC(g_hCompDC);
        HBITMAP hTmpBmp = CreateCompatibleBitmap(g_hScreenDC, wndW, wndH);
        HGDIOBJ hOld = SelectObject(hTmpDC, hTmpBmp);

        if (!PrintWindow(hwnd, hTmpDC, 0x00000002)) {
            HDC hWndDC = GetWindowDC(hwnd);
            if (hWndDC) {
                BitBlt(hTmpDC, 0, 0, wndW, wndH, hWndDC, 0, 0, SRCCOPY);
                ReleaseDC(hwnd, hWndDC);
            }
        }

        BitBlt(g_hCompDC, dstX, dstY, w, h, hTmpDC, wxSrc, wySrc, SRCCOPY);
        SelectObject(hTmpDC, hOld);
        DeleteObject(hTmpBmp);
        DeleteDC(hTmpDC);
    }

    // 4. Zoom: composition buffer (srcSize²) → capture buffer (d²)
    SetStretchBltMode(g_hCapDC, HALFTONE);
    SetBrushOrgEx(g_hCapDC, 0, 0, nullptr);
    StretchBlt(g_hCapDC, 0, 0, d, d,
               g_hCompDC, 0, 0, srcSize, srcSize,
               SRCCOPY);
}

// ── Entry point ─────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartup(&gdipToken, &si, nullptr);

    LoadConfig();

    WNDCLASSEX wc = { sizeof(wc) };
    wc.lpfnWndProc  = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"ScreenMagLens";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassEx(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    g_winX = (sx - g_diameter) / 2;
    g_winY = (sy - g_diameter) / 2;

    g_hWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"ScreenMagLens", L"Magnifier",
        WS_POPUP,
        g_winX, g_winY, g_diameter, g_diameter,
        nullptr, nullptr, hInst, nullptr);

    // Try modern API (Win10 2004+); fall back to per-window capture
    if (!SetWindowDisplayAffinity(g_hWnd, WDA_EXCLUDEFROMCAPTURE)) {
        g_useFallback = true;
    }

    DWMNCRENDERINGPOLICY ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(g_hWnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, hInst, 0);

    RegisterHotKey(g_hWnd, HK_TOGGLE,   MOD_CONTROL | MOD_ALT, 'M');
    RegisterHotKey(g_hWnd, HK_ZOOM_IN,  MOD_CONTROL | MOD_ALT, VK_OEM_PLUS);
    RegisterHotKey(g_hWnd, HK_ZOOM_OUT, MOD_CONTROL | MOD_ALT, VK_OEM_MINUS);
    RegisterHotKey(g_hWnd, HK_QUIT,     MOD_CONTROL | MOD_ALT, 'Q');

    CreateCachedResources();
    RenderBorderCache();

    ShowWindow(g_hWnd, SW_SHOWNOACTIVATE);
    SetTimer(g_hWnd, TIMER_REFRESH, FPS_INTERVAL, nullptr);
    CaptureAndRender();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    DestroyCachedResources();
    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}

// ── Point-in-lens test ──────────────────────────────────────────────────
bool IsPointInLens(int px, int py) {
    float cx = g_winX + g_diameter / 2.0f;
    float cy = g_winY + g_diameter / 2.0f;
    float dx = px - cx;
    float dy = py - cy;
    return (dx * dx + dy * dy) <= (g_diameter / 2.0f) * (g_diameter / 2.0f);
}

// ── Low-level mouse hook ────────────────────────────────────────────────
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wP, LPARAM lP) {
    if (nCode >= 0 && g_visible) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lP;

        switch (wP) {
        case WM_LBUTTONDOWN:
            if (!g_dragging && IsPointInLens(ms->pt.x, ms->pt.y)) {
                g_dragging      = true;
                g_dragConfirmed = false;
                g_dragIsLeft    = true;
                g_dragStart     = ms->pt;
                g_dragWinX      = g_winX;
                g_dragWinY      = g_winY;
                return 1;
            }
            break;

        case WM_RBUTTONDOWN:
            if (!g_dragging && IsPointInLens(ms->pt.x, ms->pt.y)) {
                g_dragging      = true;
                g_dragConfirmed = true;
                g_dragIsLeft    = false;
                g_dragStart     = ms->pt;
                g_dragWinX      = g_winX;
                g_dragWinY      = g_winY;
                return 1;
            }
            break;

        case WM_MOUSEMOVE:
            if (g_dragging) {
                int dx = ms->pt.x - g_dragStart.x;
                int dy = ms->pt.y - g_dragStart.y;
                if (!g_dragConfirmed) {
                    if (abs(dx) >= DRAG_THRESHOLD || abs(dy) >= DRAG_THRESHOLD)
                        g_dragConfirmed = true;
                    else
                        break;
                }
                g_winX = g_dragWinX + dx;
                g_winY = g_dragWinY + dy;
                SetWindowPos(g_hWnd, nullptr, g_winX, g_winY, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            }
            break;

        case WM_LBUTTONUP:
            if (g_dragging && g_dragIsLeft) {
                bool wasDrag = g_dragConfirmed;
                g_dragging = false;
                g_dragConfirmed = false;
                if (!wasDrag) break;
                return 1;
            }
            break;

        case WM_RBUTTONUP:
            if (g_dragging && !g_dragIsLeft) {
                g_dragging = false;
                g_dragConfirmed = false;
                return 1;
            }
            break;

        case WM_MOUSEWHEEL:
            if (IsPointInLens(ms->pt.x, ms->pt.y)) {
                short delta = (short)HIWORD(ms->mouseData);
                float oldZoom = g_zoom;
                if (delta > 0 && g_zoom < ZOOM_MAX) g_zoom += 0.25f;
                if (delta < 0 && g_zoom > ZOOM_MIN) g_zoom -= 0.25f;
                if (g_zoom != oldZoom) g_borderDirty = true;
                return 1;
            }
            break;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wP, lP);
}

// ── GDI resources ───────────────────────────────────────────────────────
void CreateCachedResources() {
    DestroyCachedResources();
    g_hScreenDC = GetDC(nullptr);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = g_diameter;
    bmi.bmiHeader.biHeight      = -g_diameter;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    // Output DIB (32-bit ARGB)
    g_hDIB   = CreateDIBSection(g_hScreenDC, &bmi, DIB_RGB_COLORS, &g_pBits, nullptr, 0);
    g_hMemDC = CreateCompatibleDC(g_hScreenDC);
    SelectObject(g_hMemDC, g_hDIB);

    // Border cache DIB
    g_hBorderBmp = CreateDIBSection(g_hScreenDC, &bmi, DIB_RGB_COLORS, &g_pBorderBits, nullptr, 0);
    g_hBorderDC  = CreateCompatibleDC(g_hScreenDC);
    SelectObject(g_hBorderDC, g_hBorderBmp);

    // Zoomed capture buffer (DDB)
    g_hCapBmp = CreateCompatibleBitmap(g_hScreenDC, g_diameter, g_diameter);
    g_hCapDC  = CreateCompatibleDC(g_hScreenDC);
    SelectObject(g_hCapDC, g_hCapBmp);

    // 1:1 composition buffer for per-window capture (max srcSize = d at zoom 1x)
    g_hCompBmp = CreateCompatibleBitmap(g_hScreenDC, g_diameter, g_diameter);
    g_hCompDC  = CreateCompatibleDC(g_hScreenDC);
    SelectObject(g_hCompDC, g_hCompBmp);

    BuildCircleMask();
    g_borderDirty = true;
}

void DestroyCachedResources() {
    if (g_hMemDC)     { DeleteDC(g_hMemDC);         g_hMemDC     = nullptr; }
    if (g_hDIB)       { DeleteObject(g_hDIB);        g_hDIB       = nullptr; }
    if (g_hBorderDC)  { DeleteDC(g_hBorderDC);       g_hBorderDC  = nullptr; }
    if (g_hBorderBmp) { DeleteObject(g_hBorderBmp);  g_hBorderBmp = nullptr; }
    if (g_hCapDC)     { DeleteDC(g_hCapDC);          g_hCapDC     = nullptr; }
    if (g_hCapBmp)    { DeleteObject(g_hCapBmp);     g_hCapBmp    = nullptr; }
    if (g_hCompDC)    { DeleteDC(g_hCompDC);         g_hCompDC    = nullptr; }
    if (g_hCompBmp)   { DeleteObject(g_hCompBmp);    g_hCompBmp   = nullptr; }
    if (g_hScreenDC)  { ReleaseDC(nullptr, g_hScreenDC); g_hScreenDC = nullptr; }
    delete[] g_circleMask; g_circleMask = nullptr;
    delete[] g_rowSpans;   g_rowSpans   = nullptr;
}

// ── Pre-compute circular alpha mask + row spans ─────────────────────────
void BuildCircleMask() {
    delete[] g_circleMask;
    delete[] g_rowSpans;
    int d = g_diameter;
    g_circleMask = new BYTE[d * d];
    g_rowSpans   = new RowSpan[d];
    float r = d / 2.0f;
    float b = (float)g_border;
    float innerR = r - b;

    for (int y = 0; y < d; y++) {
        float dy = y + 0.5f - r;
        int x0 = d, x1 = 0;
        for (int x = 0; x < d; x++) {
            float dx = x + 0.5f - r;
            float dist = sqrtf(dx * dx + dy * dy);
            BYTE a;
            if (dist <= innerR - 0.5f)      a = 255;
            else if (dist >= innerR + 0.5f)  a = 0;
            else a = (BYTE)((innerR + 0.5f - dist) * 255.0f);
            g_circleMask[y * d + x] = a;
            if (a > 0) { if (x < x0) x0 = x; x1 = x + 1; }
        }
        g_rowSpans[y] = { x0, x1 };
    }
}

// ── Pre-render border overlay ───────────────────────────────────────────
void RenderBorderCache() {
    Gdiplus::Graphics gfx(g_hBorderDC);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    gfx.Clear(Gdiplus::Color(0, 0, 0, 0));

    float d = (float)g_diameter;
    float b = (float)g_border;

    Gdiplus::Pen shadowPen(Gdiplus::Color(80, 0, 0, 0), 6.0f);
    gfx.DrawEllipse(&shadowPen, 3.0f, 3.0f, d - 6.0f, d - 6.0f);

    Gdiplus::Pen borderPen(Gdiplus::Color(230, 30, 30, 30), b);
    float bh = b / 2.0f;
    gfx.DrawEllipse(&borderPen, bh, bh, d - b, d - b);

    Gdiplus::Pen innerPen(Gdiplus::Color(50, 255, 255, 255), 1.0f);
    gfx.DrawEllipse(&innerPen, b, b, d - 2 * b, d - 2 * b);

    Gdiplus::Pen outerPen(Gdiplus::Color(70, 255, 255, 255), 1.0f);
    gfx.DrawEllipse(&outerPen, 0.5f, 0.5f, d - 1.0f, d - 1.0f);

    float mid = d / 2.0f;
    Gdiplus::Pen crossPen(Gdiplus::Color(90, 255, 50, 50), 1.0f);
    gfx.DrawLine(&crossPen, mid - 7, mid, mid + 7, mid);
    gfx.DrawLine(&crossPen, mid, mid - 7, mid, mid + 7);

    wchar_t zoomText[16];
    _snwprintf(zoomText, 16, L"%.1fx", g_zoom);
    Gdiplus::Font font(L"Segoe UI", 7.5f, Gdiplus::FontStyleBold);
    Gdiplus::SolidBrush textBg(Gdiplus::Color(160, 0, 0, 0));
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(200, 255, 255, 255));
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF pillRect(mid - 18, b + 2, 36.0f, 14.0f);
    gfx.FillRectangle(&textBg, pillRect);
    gfx.DrawString(zoomText, -1, &font, pillRect, &sf, &textBrush);

    Gdiplus::Font hintFont(L"Segoe UI", 6.5f, Gdiplus::FontStyleRegular);
    Gdiplus::SolidBrush hintBrush(Gdiplus::Color(120, 255, 255, 255));
    Gdiplus::RectF hintRect(mid - 30, d - b - 16, 60.0f, 14.0f);
    gfx.FillRectangle(&textBg, hintRect);
    gfx.DrawString(L"LMB/RMB drag", -1, &hintFont, hintRect, &sf, &hintBrush);

    g_borderDirty = false;
    g_lastRenderedZoom = g_zoom;
}

// ── Capture + composite ─────────────────────────────────────────────────
void CaptureAndRender() {
    if (!g_visible) return;

    int d = g_diameter;
    int cx = g_winX + d / 2;
    int cy = g_winY + d / 2;
    int srcSize = (int)(d / g_zoom);
    if (srcSize < 1) srcSize = 1;

    if (g_useFallback) {
        // Per-window capture: no self-capture, no flicker
        CaptureByWindowEnum(cx, cy, srcSize);
    } else {
        // Direct screen capture (WDA_EXCLUDEFROMCAPTURE active)
        HDC hScreen = GetDC(nullptr);
        SetStretchBltMode(g_hCapDC, HALFTONE);
        SetBrushOrgEx(g_hCapDC, 0, 0, nullptr);
        StretchBlt(g_hCapDC, 0, 0, d, d,
                   hScreen,
                   cx - srcSize / 2, cy - srcSize / 2, srcSize, srcSize,
                   SRCCOPY);
        ReleaseDC(nullptr, hScreen);
    }

    BitBlt(g_hMemDC, 0, 0, d, d, g_hCapDC, 0, 0, SRCCOPY);

    if (g_borderDirty || g_lastRenderedZoom != g_zoom)
        RenderBorderCache();

    // Apply circle mask + composite border
    {
        DWORD* px        = (DWORD*)g_pBits;
        const DWORD* brd = (const DWORD*)g_pBorderBits;
        const BYTE*  mask = g_circleMask;

        for (int y = 0; y < d; y++) {
            int rowOff = y * d;
            int x0 = g_rowSpans[y].x0;
            int x1 = g_rowSpans[y].x1;

            if (x0 >= x1) { memset(px + rowOff, 0, d * 4); continue; }
            if (x0 > 0)   memset(px + rowOff, 0, x0 * 4);
            if (x1 < d)   memset(px + rowOff + x1, 0, (d - x1) * 4);

            for (int x = x0; x < x1; x++) {
                int idx = rowOff + x;
                BYTE ma = mask[idx];

                DWORD cpx = px[idx];
                BYTE cr = (BYTE)(cpx);
                BYTE cg = (BYTE)(cpx >> 8);
                BYTE cb = (BYTE)(cpx >> 16);
                BYTE dr, dg, db, da;

                if (ma == 255) { dr = cr; dg = cg; db = cb; da = 255; }
                else {
                    dr = (BYTE)((cr * ma) / 255);
                    dg = (BYTE)((cg * ma) / 255);
                    db = (BYTE)((cb * ma) / 255);
                    da = ma;
                }

                DWORD bpx = brd[idx];
                BYTE sa = (BYTE)(bpx >> 24);
                if (sa > 0) {
                    if (sa == 255) { px[idx] = bpx; continue; }
                    BYTE invSa = 255 - sa;
                    dr = (BYTE)(((BYTE)(bpx)      * sa + dr * invSa) / 255);
                    dg = (BYTE)(((BYTE)(bpx >> 8)  * sa + dg * invSa) / 255);
                    db = (BYTE)(((BYTE)(bpx >> 16) * sa + db * invSa) / 255);
                    da = (BYTE)(sa + (da * invSa) / 255);
                }
                px[idx] = dr | (dg << 8) | (db << 16) | (da << 24);
            }
        }
    }

    POINT ptPos = { g_winX, g_winY };
    SIZE sz     = { d, d };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hWnd, nullptr, &ptPos, &sz, g_hMemDC, &ptSrc, 0, &blend, ULW_ALPHA);
}

// ── Window procedure ────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wP, LPARAM lP) {
    switch (msg) {
    case WM_TIMER:
        if (wP == TIMER_REFRESH) CaptureAndRender();
        return 0;

    case WM_HOTKEY:
        switch (wP) {
        case HK_TOGGLE:
            g_visible = !g_visible;
            ShowWindow(g_hWnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            return 0;
        case HK_ZOOM_IN:
            if (g_zoom < ZOOM_MAX) { g_zoom += 0.5f; g_borderDirty = true; }
            return 0;
        case HK_ZOOM_OUT:
            if (g_zoom > ZOOM_MIN) { g_zoom -= 0.5f; g_borderDirty = true; }
            return 0;
        case HK_QUIT:
            PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_REFRESH);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wP, lP);
}
