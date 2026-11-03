@echo off
rem Build the Windows probes with MSVC.
rem   probes\windows\build.cmd C:\path\to\oorexx [C:\path\to\Apache24]
rem   (the ooRexx installer, extracted; Apache, for the probe module)
rem Copyright 2026 Josep Maria Blasco -- Apache License 2.0
setlocal
set OO=%~1
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo vswhere found no Visual C++ & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /W3 /EHsc /O2 /I"%OO%\api" api.cpp /link "%OO%\api\rexx.lib" "%OO%\api\rexxapi.lib" /OUT:api.exe || exit /b 1
cl /nologo /W3 /EHsc /O2 /I"%OO%\api" pipestream.cpp /link "%OO%\api\rexx.lib" "%OO%\api\rexxapi.lib" /OUT:pipestream.exe || exit /b 1
cl /nologo /W3 /O2 transport.c /link ws2_32.lib advapi32.lib /OUT:transport.exe || exit /b 1
if "%~2"=="" goto done
set AP=%~2
cl /nologo /W3 /O2 /LD /I"%AP%\include" mod_orxprobe.c /link "%AP%\lib\libhttpd.lib" "%AP%\lib\libapr-1.lib" "%AP%\lib\libaprutil-1.lib" ws2_32.lib /OUT:mod_orxprobe.so || exit /b 1
:done
echo built
