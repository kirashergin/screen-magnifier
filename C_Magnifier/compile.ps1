$ErrorActionPreference = "Continue"
$cl = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe'
$dir = 'C:\Users\73B5~1\source\repos\CSharp_AxetechOS\Magnifier'
$sdkVer = '10.0.26100.0'
$sdkBase = "C:\Program Files (x86)\Windows Kits\10"
$msvcBase = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"

$env:INCLUDE = "$msvcBase\include;$sdkBase\Include\$sdkVer\um;$sdkBase\Include\$sdkVer\shared;$sdkBase\Include\$sdkVer\ucrt"
$env:LIB = "$msvcBase\lib\x64;$sdkBase\Lib\$sdkVer\um\x64;$sdkBase\Lib\$sdkVer\ucrt\x64"
$env:PATH = "$msvcBase\bin\Hostx64\x64;$env:PATH"

Set-Location $dir

Write-Host "Compiling magnifier.c ..."
$proc = Start-Process -FilePath $cl `
    -ArgumentList "/nologo /O2 /GS- /W3 /DWIN32 /DNDEBUG /D_WINDOWS magnifier.c /Fe:magnifier.exe /link /SUBSYSTEM:WINDOWS magnification.lib user32.lib gdi32.lib shell32.lib" `
    -NoNewWindow -Wait -PassThru
Write-Host "Exit code: $($proc.ExitCode)"
if (Test-Path "$dir\magnifier.exe") {
    Write-Host "SUCCESS: magnifier.exe created"
    $info = Get-Item "$dir\magnifier.exe"
    Write-Host "Size: $($info.Length) bytes"
} else {
    Write-Host "FAILED: magnifier.exe not found"
}
Remove-Item "$dir\magnifier.obj" -ErrorAction SilentlyContinue
