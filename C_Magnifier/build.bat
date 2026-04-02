@echo off
:: Сборка magnifier.exe через MSVC
:: Требует: Visual Studio 2019/2022 + Windows SDK

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0compile.ps1"
pause
