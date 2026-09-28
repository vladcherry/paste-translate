@echo off
rem Builds build\PasteTranslate.exe with MSVC. Usage: build.bat
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto :icon
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :nomsvc
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :nomsvc
rem vcvars prints harmless noise on stderr on some installs; cl is verified below.
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl >nul 2>nul || goto :nomsvc

:icon
rem icon.ico is generated, not committed. Needs Python 3 + Pillow.
if exist icon.ico goto :build
python gen_icon.py || exit /b 1
if not exist icon.ico echo gen_icon.py did not produce icon.ico & exit /b 1

:build
if not exist build mkdir build
rc /nologo /fo build\translator.res translator.rc || exit /b 1
cl /nologo /O1 /GS- /Gy /MT /W3 /DNDEBUG translator.c build\translator.res /Fo:build\ /Fe:build\PasteTranslate.exe ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO /INCREMENTAL:NO ^
   user32.lib gdi32.lib comctl32.lib shell32.lib winhttp.lib || exit /b 1
echo Built build\PasteTranslate.exe
exit /b 0

:nomsvc
echo MSVC not found. Install "Build Tools for Visual Studio" with the "Desktop development with C++" workload.
exit /b 1
