/*
 * magnifier.c  --  круглая лупа на Windows (Win32 + Magnification API)
 *
 * Лупа перетаскивается вручную (тач / мышь). Показывает увеличенную
 * область экрана под собой. Клики вне круга проходят к интерфейсу.
 *
 * Запуск:
 *   magnifier.exe [--radius=N] [--zoom=F] [--fps=N]
 *
 * Конфиг (magnifier.ini рядом с .exe):
 *   radius=150
 *   zoom=2.0
 *   fps=30
 *
 * Закрыть: Escape
 */

#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0601
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <magnification.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma warning(disable: 4996)

#pragma comment(lib, "magnification.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

/* ------------------------------------------------------------------ */
/* DPI awareness                                                       */
/* ------------------------------------------------------------------ */
static void SetDpiAware(void)
{
    typedef BOOL (WINAPI *PFN_SPDAC)(DPI_AWARENESS_CONTEXT);
    HMODULE hUser = GetModuleHandleA("user32.dll");
    if (hUser) {
        PFN_SPDAC fn = (PFN_SPDAC)GetProcAddress(hUser,
                            "SetProcessDpiAwarenessContext");
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            return;
    }
    typedef HRESULT (WINAPI *PFN_SPDA)(int);
    HMODULE hShcore = LoadLibraryA("shcore.dll");
    if (hShcore) {
        PFN_SPDA fn = (PFN_SPDA)GetProcAddress(hShcore,
                            "SetProcessDpiAwareness");
        if (fn && SUCCEEDED(fn(2)))
            { FreeLibrary(hShcore); return; }
        FreeLibrary(hShcore);
    }
    if (hUser) {
        typedef BOOL (WINAPI *PFN_SPDAW)(void);
        PFN_SPDAW fn = (PFN_SPDAW)GetProcAddress(hUser,
                            "SetProcessDPIAware");
        if (fn) fn();
    }
}

/* ------------------------------------------------------------------ */
static int   g_radius = 150;
static float g_zoom   = 2.0f;
static int   g_fps    = 30;

static HWND  g_hwndHost = NULL;
static HWND  g_hwndMag  = NULL;

/* Перетаскивание */
static BOOL  g_dragging = FALSE;
static POINT g_dragStart;       /* позиция курсора (screen) при начале drag */
static POINT g_wndStart;        /* позиция окна при начале drag */

#define HOTKEY_QUIT  1
#define TIMER_RENDER 1

/* ------------------------------------------------------------------ */
/* Обновление источника — показываем область экрана под центром окна   */
/* ------------------------------------------------------------------ */
static void RefreshSourceFromWindowPos(void)
{
    RECT wr;
    GetWindowRect(g_hwndHost, &wr);
    int cx = (wr.left + wr.right)  / 2;
    int cy = (wr.top  + wr.bottom) / 2;

    int d  = g_radius * 2;
    int sw = (int)((float)d / g_zoom);
    int sh = (int)((float)d / g_zoom);
    RECT src = { cx - sw/2, cy - sh/2, cx + sw/2, cy + sh/2 };
    MagSetWindowSource(g_hwndMag, src);
    InvalidateRect(g_hwndMag, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Оконная процедура                                                   */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    /* ── Начало перетаскивания (мышь) ─────────────────────────── */
    case WM_LBUTTONDOWN: {
        g_dragging = TRUE;
        SetCapture(hwnd);
        GetCursorPos(&g_dragStart);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        g_wndStart.x = wr.left;
        g_wndStart.y = wr.top;
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!g_dragging) break;
        POINT cur;
        GetCursorPos(&cur);
        int nx = g_wndStart.x + (cur.x - g_dragStart.x);
        int ny = g_wndStart.y + (cur.y - g_dragStart.y);
        SetWindowPos(hwnd, NULL, nx, ny, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        RefreshSourceFromWindowPos();
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_dragging) {
            g_dragging = FALSE;
            ReleaseCapture();
        }
        return 0;
    }

    /* ── Фоновый таймер: обновляем контент под лупой ─────────── */
    case WM_TIMER: {
        RefreshSourceFromWindowPos();
        return 0;
    }

    case WM_HOTKEY:
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Чтение .ini                                                         */
/* ------------------------------------------------------------------ */
static void ReadIni(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *p = strrchr(path, '\\');
    if (p) strcpy(p + 1, "magnifier.ini");
    else   return;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '#') continue;
        int iv; float fv;
        if (sscanf(line, "radius=%d", &iv) == 1) { g_radius = iv; continue; }
        if (sscanf(line, "zoom=%f",   &fv) == 1) { g_zoom   = fv; continue; }
        if (sscanf(line, "fps=%d",    &iv) == 1) { g_fps    = iv; continue; }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Разбор аргументов командной строки                                  */
/* ------------------------------------------------------------------ */
static void ParseArgs(void)
{
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;

    for (int i = 1; i < argc; i++) {
        char buf[64];
        WideCharToMultiByte(CP_ACP, 0, argv[i], -1, buf, sizeof(buf), NULL, NULL);

        int iv; float fv;
        if      (sscanf(buf, "--radius=%d", &iv) == 1) g_radius = iv;
        else if (sscanf(buf, "--zoom=%f",   &fv) == 1) g_zoom   = fv;
        else if (sscanf(buf, "--fps=%d",    &iv) == 1) g_fps    = iv;
        else if (strcmp(buf, "-r") == 0 && i+1 < argc) {
            WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, buf, sizeof(buf), NULL, NULL);
            g_radius = atoi(buf);
        }
        else if (strcmp(buf, "-z") == 0 && i+1 < argc) {
            WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, buf, sizeof(buf), NULL, NULL);
            g_zoom = (float)atof(buf);
        }
    }
    LocalFree(argv);
}

/* ------------------------------------------------------------------ */
/* WinMain                                                             */
/* ------------------------------------------------------------------ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR lpCmd, int nShow)
{
    (void)hPrev; (void)lpCmd; (void)nShow;

    SetDpiAware();

    ReadIni();
    ParseArgs();

    if (g_radius < 20)    g_radius = 20;
    if (g_radius > 800)   g_radius = 800;
    if (g_zoom   < 1.1f)  g_zoom   = 1.1f;
    if (g_zoom   > 20.0f) g_zoom   = 20.0f;
    if (g_fps    < 5)     g_fps    = 5;
    if (g_fps    > 60)    g_fps    = 60;

    if (!MagInitialize()) {
        MessageBoxA(NULL,
            "Magnification API недоступен.\nТребуется Windows 7 или новее.",
            "Magnifier", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = HostProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_SIZEALL);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "MagHost";
    if (!RegisterClassExA(&wc)) { MagUninitialize(); return 1; }

    int d = g_radius * 2;

    /* Начальная позиция — центр экрана */
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int startX = (sx - d) / 2;
    int startY = (sy - d) / 2;

    /*
     * WS_EX_TOPMOST  — поверх всех окон
     * WS_EX_LAYERED  — для SetLayeredWindowAttributes
     * БЕЗ WS_EX_TRANSPARENT — окно получает ввод (для перетаскивания)
     * БЕЗ WS_EX_NOACTIVATE — чтобы получать mouse/touch события
     */
    g_hwndHost = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED,
        "MagHost", NULL,
        WS_POPUP | WS_VISIBLE,
        startX, startY, d, d,
        NULL, NULL, hInst, NULL);

    if (!g_hwndHost) { MagUninitialize(); return 1; }

    SetLayeredWindowAttributes(g_hwndHost, 0, 255, LWA_ALPHA);

    /* Круглая форма — клики вне круга проходят к интерфейсу */
    HRGN hRgn = CreateEllipticRgn(0, 0, d, d);
    SetWindowRgn(g_hwndHost, hRgn, FALSE);

    g_hwndMag = CreateWindowA(
        WC_MAGNIFIER, NULL,
        WS_CHILD | WS_VISIBLE,
        0, 0, d, d,
        g_hwndHost, NULL, hInst, NULL);

    if (!g_hwndMag) {
        DestroyWindow(g_hwndHost);
        MagUninitialize();
        return 1;
    }

    MAGTRANSFORM mt;
    ZeroMemory(&mt, sizeof(mt));
    mt.v[0][0] = g_zoom;
    mt.v[1][1] = g_zoom;
    mt.v[2][2] = 1.0f;
    MagSetWindowTransform(g_hwndMag, &mt);

    /* Таймер для обновления контента под лупой */
    SetTimer(g_hwndHost, TIMER_RENDER, 1000 / g_fps, NULL);

    RegisterHotKey(g_hwndHost, HOTKEY_QUIT, 0, VK_ESCAPE);

    RefreshSourceFromWindowPos();
    ShowWindow(g_hwndHost, SW_SHOW);
    UpdateWindow(g_hwndHost);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(g_hwndHost, TIMER_RENDER);
    UnregisterHotKey(g_hwndHost, HOTKEY_QUIT);
    MagUninitialize();
    return (int)msg.wParam;
}
