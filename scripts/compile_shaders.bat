@echo off
REM Compiles every .sc shader in assets/shaders/src with bgfx's shaderc, once per
REM backend profile, into each app's assets/shaders/compiled/<profile>/ folder.
REM
REM Shaders are no longer compiled at runtime: bgfx consumes pre-compiled
REM bytecode, so this has to run after any shader edit. Run
REM scripts/build_shader_tools.bat first if shaderc.exe is missing.
REM
REM Profile -> folder mapping must match ProfileDirectory() in Shader.cpp.

setlocal enabledelayedexpansion

set ROOT=%~dp0..
set SRC=%ROOT%\assets\shaders\src
set SHADERC=%ROOT%\scripts\tools\windows\shaderc.exe
set INCLUDE=%ROOT%\GanymedEngine\extern\bgfx\src

if not exist "%SHADERC%" (
	echo [ERROR] shaderc not found at %SHADERC%
	echo         Run scripts\build_shader_tools.bat first.
	exit /b 1
)

REM Every app that loads shaders at runtime gets its own compiled copy,
REM because assets are resolved relative to the working directory.
set TARGETS=%ROOT%\GanymedEditor\assets\shaders\compiled %ROOT%\Sandbox\assets\shaders\compiled

set FAILED=0
set BUILT=0

for %%T in (%TARGETS%) do (
	call :compile_profile "%%T" dx11  s_5_0
	call :compile_profile "%%T" spirv spirv
	call :compile_profile "%%T" glsl  410
)

echo.
if %FAILED%==0 (
	echo Compiled %BUILT% shader binaries.
) else (
	echo [ERROR] %FAILED% shader^(s^) failed to compile.
	exit /b 1
)
endlocal
exit /b 0

:compile_profile
REM %1 = target compiled/ root, %2 = profile folder, %3 = shaderc profile
set OUTDIR=%~1\%2
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

for %%F in ("%SRC%\vs_*.sc") do (
	call :varyingdef "%%~nF"
	"%SHADERC%" -f "%%F" -o "%OUTDIR%\%%~nF.bin" --type vertex --platform windows ^
		-p %3 -i "%INCLUDE%" --varyingdef "!VARYING!"
	if errorlevel 1 ( echo   FAILED %2/%%~nF & set /a FAILED+=1 ) else ( set /a BUILT+=1 )
)

for %%F in ("%SRC%\fs_*.sc") do (
	call :varyingdef "%%~nF"
	"%SHADERC%" -f "%%F" -o "%OUTDIR%\%%~nF.bin" --type fragment --platform windows ^
		-p %3 -i "%INCLUDE%" --varyingdef "!VARYING!"
	if errorlevel 1 ( echo   FAILED %2/%%~nF & set /a FAILED+=1 ) else ( set /a BUILT+=1 )
)
exit /b 0

:varyingdef
REM Shaders whose attributes differ from the engine's standard vertex layout
REM (ImGui, for one) can ship varying.<name>.def.sc and it is preferred.
REM %1 is the file stem, e.g. "vs_ImGui" -> name "ImGui".
set STEM=%~1
set NAME=!STEM:~3!
if exist "%SRC%\varying.!NAME!.def.sc" (
	set VARYING=%SRC%\varying.!NAME!.def.sc
) else (
	set VARYING=%SRC%\varying.def.sc
)
exit /b 0
