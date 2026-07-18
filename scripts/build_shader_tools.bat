@echo off
REM Builds bgfx's shaderc (offline shader compiler) and stages it in scripts/tools/windows.
REM
REM shaderc is built with bgfx's own GENie project rather than our premake5 setup:
REM it pulls in glslang, spirv-tools, spirv-cross, glsl-optimizer, fcpp and tint,
REM and re-describing that dependency graph in premake would be a large, fragile
REM duplication for a tool that is built once and rarely changes.
REM
REM The binaries are gitignored (*.exe), so run this once per machine / after a
REM bgfx submodule update.

setlocal enabledelayedexpansion

set ROOT=%~dp0..
set BGFX=%ROOT%\GanymedEngine\extern\bgfx
set OUT=%ROOT%\scripts\tools\windows

if not exist "%BGFX%\src\amalgamated.cpp" (
	echo [ERROR] bgfx submodule not checked out. Run: git submodule update --init --recursive
	exit /b 1
)

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
	echo [ERROR] vswhere.exe not found - is Visual Studio installed?
	exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if "%MSBUILD%"=="" (
	echo [ERROR] MSBuild.exe not found.
	exit /b 1
)

echo [1/3] Generating bgfx tool projects with GENie...
pushd "%BGFX%"
call "..\bx\tools\bin\windows\genie.exe" --with-tools vs2022 >nul
if errorlevel 1 (
	echo [ERROR] GENie failed.
	popd
	exit /b 1
)

echo [2/3] Building shaderc (Release x64)... this takes a few minutes.
call "%MSBUILD%" .build\projects\vs2022\shaderc.vcxproj /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if errorlevel 1 (
	echo [ERROR] shaderc build failed.
	popd
	exit /b 1
)
popd

echo [3/3] Staging into scripts\tools\windows...
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%BGFX%\.build\win64_vs2022\bin\shadercRelease.exe" "%OUT%\shaderc.exe" >nul

REM shaderc loads these at runtime for the D3D/DXIL profiles.
copy /y "%BGFX%\tools\bin\windows\d3dcompiler_47.dll" "%OUT%\" >nul
copy /y "%BGFX%\tools\bin\windows\dxcompiler.dll" "%OUT%\" >nul
copy /y "%BGFX%\tools\bin\windows\dxil.dll" "%OUT%\" >nul

echo.
echo Done: %OUT%\shaderc.exe
"%OUT%\shaderc.exe" --version
endlocal
