@echo off
pushd %~dp0\..\

echo GanymedEngine - Cross-Platform Project Generator
echo ================================================
echo.

REM Check if we're running in a Unix-like environment (Git Bash, WSL, etc.)
where bash >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo Detected Bash environment, running cross-platform script...
    bash scripts/GenerateProjects.sh
    goto :end
)

REM Fallback to Windows-specific generation
echo Running Windows-specific project generation...
echo.

if not exist "vendor\premake\bin\premake5.exe" (
    echo Error: premake5.exe not found in vendor\premake\bin\
    echo Please ensure Premake5 is installed in the vendor directory.
    echo You can download it from: https://premake.github.io/download.html
    pause
    exit /b 1
)

echo Generating Visual Studio 2022 solution...
call vendor\premake\bin\premake5.exe vs2022

if %ERRORLEVEL% equ 0 (
    echo.
    echo Project files generated successfully!
    echo.
    echo Build Instructions:
    echo ==================
    echo   • Open GanymedEngine.sln in Visual Studio 2022
    echo   • Or use MSBuild from command line
    echo.
    echo Available configurations: Debug, Release, Dist
) else (
    echo.
    echo Error: Failed to generate project files!
)

:end
popd
PAUSE