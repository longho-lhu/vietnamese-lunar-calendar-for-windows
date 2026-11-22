@echo off
rem ==========================================================================
rem  Bien dich AmLichTap.dll + AmLichVN.exe bang Visual Studio 2022 (MSVC).
rem  Yeu cau: Visual Studio 2022 (ban Community mien phi) voi workload
rem  "Desktop development with C++" (co san Windows 11 SDK).
rem  Ket qua: thu muc dist\
rem ==========================================================================
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto :have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 goto :no_vs

:have_cl
if exist build rmdir /s /q build
if exist dist rmdir /s /q dist
mkdir build
mkdir dist

set CFLAGS=/nologo /std:c++20 /EHsc /permissive- /O2 /MT /W3 /utf-8 /bigobj /DUNICODE /D_UNICODE /DNOMINMAX

echo === Bien dich AmLichTap.dll ===
cl %CFLAGS% /LD src\tap\AmLichTap.cpp /Fobuild\ /Fe:dist\AmLichTap.dll ^
   /link /DEF:src\tap\AmLichTap.def runtimeobject.lib ole32.lib oleaut32.lib advapi32.lib user32.lib
if errorlevel 1 goto :fail

echo === Bien dich AmLichVN.exe ===
cl %CFLAGS% src\app\AmLichVN.cpp /Fobuild\ /Fe:dist\AmLichVN.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED shell32.lib user32.lib gdi32.lib advapi32.lib ^
   ole32.lib comctl32.lib comdlg32.lib winhttp.lib bcrypt.lib
if errorlevel 1 goto :fail

copy /y settings.default.ini dist\ >nul
copy /y scripts\*.ps1 dist\ >nul
copy /y scripts\*.cmd dist\ >nul
del /q dist\*.exp dist\*.lib 2>nul

echo.
echo === XONG. Cac file nam trong thu muc dist\ ===
echo Chay "dist\CaiDat.cmd" de cai dat.
exit /b 0

:no_vs
echo [LOI] Khong tim thay Visual Studio 2022 co cong cu C++ x64.
echo Hay cai Visual Studio 2022 Community, chon workload "Desktop development with C++".
exit /b 1

:fail
echo.
echo [LOI] Bien dich that bai. Xem thong bao loi o tren.
exit /b 1
