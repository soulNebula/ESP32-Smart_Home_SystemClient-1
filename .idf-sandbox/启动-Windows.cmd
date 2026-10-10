@echo off
REM ===========================================================================
REM  ESP32 sandbox -- WINDOWS launcher  (double-click this)
REM
REM  On Linux use  start-linux.sh  instead.
REM
REM  It picks the best Python it can find, in this order:
REM    1) the sandbox's own pythonw.exe   (best: no console window, GUI ready)
REM    2) the sandbox's own python.exe
REM    3) this PC's Python launcher  (py.exe)
REM    4) any python.exe on PATH
REM
REM  A fresh copy from GitHub has no Python inside the sandbox -- step 3/4
REM  keeps the double-click working anyway, and the window you get will
REM  offer "configure the sandbox", which downloads a proper portable
REM  Python (with tkinter) into .idf-sandbox\python\.
REM ===========================================================================
setlocal EnableExtensions
chcp 65001 >nul 2>&1

set "SB=%~dp0"
set "SCRIPT=%SB%start.py"
set "PYW=%SB%python\pythonw.exe"
set "PY=%SB%python\python.exe"

if not exist "%SCRIPT%" goto :no_script

REM --- 1) sandbox's own pythonw: start detached, this console goes away ------
if exist "%PYW%" (
    start "" "%PYW%" "%SCRIPT%" --os windows %*
    exit /b 0
)

REM --- 2) sandbox's own python.exe ------------------------------------------
if exist "%PY%" (
    "%PY%" "%SCRIPT%" --os windows %*
    set "RC=%ERRORLEVEL%"
    if not "%RC%"=="0" pause
    exit /b %RC%
)

REM --- 3) Python launcher / 4) python on PATH -------------------------------
REM  Run in THIS console (not detached) so the first-run output is visible --
REM  that is exactly where "configure the sandbox" prints.
echo.
echo   [!] The sandbox has no Python of its own yet.
echo       Using this PC's Python for now.  The window will offer to
echo       download a proper one into .idf-sandbox\python\.
echo.

where py.exe >nul 2>&1
if not errorlevel 1 (
    py.exe -3 "%SCRIPT%" --os windows %*
    set "RC=%ERRORLEVEL%"
    if not "%RC%"=="0" pause
    exit /b %RC%
)

where python.exe >nul 2>&1
if not errorlevel 1 (
    python.exe "%SCRIPT%" --os windows %*
    set "RC=%ERRORLEVEL%"
    if not "%RC%"=="0" pause
    exit /b %RC%
)

goto :no_python

:no_python
echo.
echo   [X] No Python found on this PC, and the sandbox has none yet.
echo.
echo   Install Python once (any 3.9 - 3.14), then run this file again:
echo       https://www.python.org/downloads/windows/
echo   During setup, tick "Add python.exe to PATH".
echo.
echo   The sandbox downloads everything else by itself -- you do NOT need
echo   to install CMake, Ninja, or ESP-IDF.
echo.
pause
endlocal & exit /b 2

:no_script
echo.
echo   [X] start.py not found:
echo       %SCRIPT%
echo   Please copy the WHOLE .idf-sandbox folder again.
echo.
pause
endlocal & exit /b 2
