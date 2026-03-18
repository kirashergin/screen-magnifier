@echo off
:: Find and setup VS Build Tools environment
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist "dist" mkdir dist

cl.exe /O2 /EHsc /DUNICODE /D_UNICODE ^
    /Fe:dist\magnifier.exe ^
    src\main.cpp ^
    /link /SUBSYSTEM:WINDOWS ^
    user32.lib gdi32.lib gdiplus.lib ole32.lib dwmapi.lib

if %ERRORLEVEL%==0 (
    echo.
    echo === Build OK: dist\magnifier.exe ===
    :: Cleanup intermediate files
    del /q *.obj 2>nul
) else (
    echo.
    echo === Build FAILED ===
)
