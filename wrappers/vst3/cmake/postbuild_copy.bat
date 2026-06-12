@echo off
setlocal

:: %1 is the full path to the .vst3 bundle
set "SRC=%~1"
set "DEST=C:\Program Files\Common Files\VST3"
set "DESTDIR=%DEST%\%~nx1"

:: Check if we are running as admin
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting elevation...
    powershell -Command "Start-Process '%~f0' -ArgumentList '%SRC%' -Verb RunAs"
    exit /b
)

echo Copying VST3 plugin from:
echo     %SRC%
echo To:
echo     %DESTDIR%
xcopy /E /I /Y "%SRC%" "%DESTDIR%"
if errorlevel 1 (
    echo Failed to copy VST3 plugin.
    exit /b 1
)
echo VST3 plugin copied successfully.
exit /b 0