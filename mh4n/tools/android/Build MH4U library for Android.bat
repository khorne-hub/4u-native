@echo off
rem Builds the game's code library for the MH4U Android app from your own MH4U v1.3.0 update (see build-android-library.ps1)
rem (opened from inside a ZIP, Windows copies this file alone to a temporary folder: say so and wait, instead of a window
rem that closes at once)
if not exist "%~dp0build-android-library.ps1" (
    echo.
    echo This tool can't find its other files. Unzip ALL of the download first ^(right-click the ZIP, Extract All^),
    echo then open this file from the unzipped folder: mh4n\tools\android.
    echo.
    echo La herramienta no encuentra sus otros archivos. Descomprime ENTERA la descarga primero ^(clic derecho en el ZIP,
    echo Extraer todo^) y abre este archivo desde la carpeta descomprimida: mh4n\tools\android.
    echo.
    pause
    exit /b 1
)
rem (the window waits here whatever happened, so its last lines can be read: PowerShell may refuse to run the script at all)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-android-library.ps1" -NoPause %*
set code=%errorlevel%
echo.
pause
exit /b %code%
