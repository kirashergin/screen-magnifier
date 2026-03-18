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

- **Layered window** — A `WS_POPUP` window with `WS_EX_LAYERED | WS_EX_TRANSPARENT` for per-pixel alpha and full click-through. Uses `WDA_EXCLUDEFROMCAPTURE` so the lens doesn't capture itself.
- **Screen capture pipeline** — `StretchBlt` from the screen DC into a capture buffer, composited through GDI+ with a circular clip path, then alpha-blended with a cached border overlay. Final output via `UpdateLayeredWindow`.
- **Adaptive refresh** — Three-tier timer (60 FPS active → 10 FPS idle → 4 FPS sleep) based on cursor movement detection. The timer interval adjusts dynamically to reduce CPU when the cursor is still.
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
