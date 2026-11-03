@echo off
rem mod_orx -- build the Apache module and the Rexx host on Windows, with MSVC.
rem
rem   build.cmd OOREXX APACHE
rem
rem OOREXX is ooRexx's directory (the one with rexx.exe and api\), APACHE is
rem Apache's (the one with bin\, include\ and lib\: Apache Lounge's build,
rem say). The results go in build\: mod_orx.so, for Apache's modules\, and
rem oorexx-host.exe, which mod_orx looks for, by default, in Apache's bin\
rem (OoRexxHost says otherwise). Windows 10 1803 or Server 2019 at least.
rem
rem Copyright 2026 Josep Maria Blasco
rem Licensed under the Apache License, Version 2.0 (see LICENSE).
setlocal
set OO=%~1
set AP=%~2
if "%AP%"=="" (echo usage: build.cmd OOREXX APACHE & exit /b 2)
if not exist "%OO%\api\oorexxapi.h" (echo %OO%\api\oorexxapi.h not found & exit /b 2)
if not exist "%AP%\include\httpd.h" (echo %AP%\include\httpd.h not found & exit /b 2)

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo vswhere found no Visual C++ & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

rem Where the module looks for oorexx-host when OoRexxHost is not set
rem (relative: to ServerRoot).
> build\config.h echo #define OORX_DEFAULT_HOST "bin/oorexx-host.exe"
>> build\config.h echo #define ORX_REXX_PLUGIN "modules/orx_rexx_plugin.dll"

rem The APACHE environment, written in Rexx, built into the host as a string,
rem as the Makefile does (with LF line ends, whatever the checkout has). MSVC
rem takes no string literal longer than 16380 bytes.
powershell -NoProfile -ExecutionPolicy Bypass -File host\embed.ps1 host\apache.rex build\apache_src.h || exit /b 1

rem The host: the C runtime linked in, so that it needs nothing besides rexx.dll.
cl /nologo /W3 /EHsc /O2 /MT /I"%OO%\api" /Fobuild\ host\oorexx-host.cpp ^
   /link "%OO%\api\rexx.lib" "%OO%\api\rexxapi.lib" ws2_32.lib advapi32.lib ^
   /OUT:build\oorexx-host.exe || exit /b 1

rem The module, against Apache's own headers and import libraries.
cl /nologo /W3 /O2 /MD /LD /I"%AP%\include" /Fobuild\ module\mod_orx.c ^
   /link "%AP%\lib\libhttpd.lib" "%AP%\lib\libapr-1.lib" "%AP%\lib\libaprutil-1.lib" ^
   ws2_32.lib advapi32.lib /OUT:build\mod_orx.so || exit /b 1

echo built build\mod_orx.so and build\oorexx-host.exe
