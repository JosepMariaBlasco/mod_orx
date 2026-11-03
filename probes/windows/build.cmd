@echo off
rem Build the Windows probes with MSVC.
rem   probes\windows\build.cmd C:\path\to\oorexx     (the installer, extracted)
rem Copyright 2026 Josep Maria Blasco -- Apache License 2.0
setlocal
set OO=%~1
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo vswhere found no Visual C++ & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /W3 /EHsc /O2 /I"%OO%\api" api.cpp /link "%OO%\api\rexx.lib" "%OO%\api\rexxapi.lib" /OUT:api.exe || exit /b 1
cl /nologo /W3 /O2 transport.c /link ws2_32.lib /OUT:transport.exe || exit /b 1
echo built: api.exe transport.exe
