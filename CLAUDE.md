# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A Windows screen magnifier that renders a circular, always-on-top lens following the cursor. Single-file C++ Win32/GDI+ application with no external dependencies beyond the Windows SDK.

## Build

Requires Visual Studio 2022 Build Tools (MSVC). Run from a VS Developer Command Prompt or let the script set up vcvars:

```
build.bat
```

Output: `dist\magnifier.exe`. No project/solution files — just a direct `cl.exe` invocation.

Linked libraries: `user32.lib`, `gdi32.lib`, `gdiplus.lib`, `ole32.lib`.

## Architecture

Everything lives in `src/main.cpp` (~435 lines). Key subsystems:

- **Layered window** — A `WS_POPUP` window with `WS_EX_LAYERED | WS_EX_TRANSPARENT` for per-pixel alpha and full click-through. Uses `WDA_EXCLUDEFROMCAPTURE` so the lens doesn't capture itself; falls back to per-window capture via `EnumWindows` + `GetWindowDC` when the API is unavailable (e.g. layered window limitation on some Win10 systems).
- **Screen capture pipeline** — Two paths: (1) direct `StretchBlt` from screen DC when `WDA_EXCLUDEFROMCAPTURE` works, (2) per-window composition that enumerates visible windows in Z-order, captures each via `GetWindowDC`/`BitBlt`, skipping the lens, then zooms via `StretchBlt`. Both feed into the same circular mask + border compositing.
- **Refresh** — Fixed 60 FPS timer (~16ms).
- **Input handling** — Low-level mouse hook (`WH_MOUSE_LL`) for right-click drag repositioning and mouse wheel zoom. Global hotkeys via `RegisterHotKey` for toggle (`Ctrl+Alt+M`), zoom (`Ctrl+Alt+±`), and quit (`Ctrl+Alt+Q`).
- **Border cache** — The decorative border/crosshair/zoom label is pre-rendered to a separate DIB and only redrawn when zoom changes, then manually alpha-composited each frame.

## Hotkeys

| Shortcut | Action |
|---|---|
| Ctrl+Alt+M | Toggle visibility |
| Ctrl+Alt+Plus/Minus | Zoom in/out (0.5x steps) |
| Ctrl+Alt+Q | Quit |
| Mouse wheel over lens | Zoom in/out (0.25x steps) |
| Right-click drag on lens | Reposition |

## TODO

- [ ] **Click-through pass-through** — Currently `WS_EX_TRANSPARENT` makes ALL clicks pass through the lens. Need selective behavior: clicks on the lens border/UI elements should be handled (drag), clicks on the magnified content area should pass through to the underlying window at the correct (un-magnified) screen coordinate. Requires removing `WS_EX_TRANSPARENT`, performing hit-testing in `WndProc` via `WM_NCHITTEST` (return `HTTRANSPARENT` for content area, `HTCLIENT` for border/drag zones), and forwarding clicks to the correct target window.
- [ ] **Touch/pen input support** — Add `WM_TOUCH` or `WM_POINTER` message handling for touchscreen interaction. Key tasks: register for touch with `RegisterTouchWindow`, handle `WM_POINTERDOWN`/`WM_POINTERUPDATE`/`WM_POINTERUP` for drag gestures, implement pinch-to-zoom via multi-touch tracking (two-finger distance delta → zoom adjustment), and ensure touch events on the content area pass through to underlying windows (similar to click-through issue above).
