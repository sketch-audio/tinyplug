@echo off
setlocal

:: %1 is the full path to the .aaxplugin folder
set "SRC=%~1"
set "DEST=C:\Program Files\Common Files\Avid\Audio\Plug-Ins"
set "DESTDIR=%DEST%\%~nx1"

:: Check if we are running as admin
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting elevation...
    powershell -Command "Start-Process '%~f0' -ArgumentList '%SRC%' -Verb RunAs"
    exit /b
)

echo Copying AAX plugin from:
echo     %SRC%
echo To:
echo     %DESTDIR%
xcopy /E /I /Y "%SRC%" "%DESTDIR%"
if errorlevel 1 (
    echo Failed to copy AAX plugin.
    exit /b 1
)
echo AAX plugin copied successfully.
exit /b 0