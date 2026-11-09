@echo off
setlocal
cd /d "%~dp0"

REM Builds nightfire.exe and nightfire_driving.exe from the disc in game\.
REM The window stays open at the end, so an error can be read.

if not exist game\default.xbe (
    echo game\default.xbe not found. Copy your extracted disc into the "game" folder first.
    goto :end
)

REM "git pull" leaves the toolkit submodule at its old commit, and the project
REM then no longer compiles against it. Bring it to the commit the project
REM names. Skipped without git or outside a clone.
where git >nul 2>&1
if not errorlevel 1 if exist .git (
    echo Updating the toolkit submodule...
    git submodule update --init --recursive
    if errorlevel 1 goto :failed
)

echo [1/2] Recompiling the game code (takes 15-30 minutes)...
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\recompile.ps1
if errorlevel 1 goto :failed

echo [2/2] Building...
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
if errorlevel 1 goto :failed

echo.
echo Done. Start the game with "Nightfire Launcher.exe".
goto :end

:failed
echo.
echo FAILED. See the message above (and build\pipeline\*\*.log).

:end
pause
