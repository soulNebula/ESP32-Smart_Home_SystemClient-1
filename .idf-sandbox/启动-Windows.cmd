@echo off
REM ===========================================================================
REM  ESP32 sandbox -- WINDOWS launcher  (double-click this on Windows)
REM
REM  On Linux, use  start-linux.sh  instead:
REM      sh .idf-sandbox/start-linux.sh
REM
REM  This just runs the sandbox's own Python on start.py --os windows.
REM  Nothing has to be installed on this PC. No PowerShell inside.
REM ===========================================================================
setlocal EnableExtensions
chcp 65001 >nul 2>&1

set "SB=%~dp0"
set "PYW=%SB%python\pythonw.exe"
set "PY=%SB%python\python.exe"
set "SCRIPT=%SB%start.py"

if not exist "%SCRIPT%" goto :no_script
if not exist "%PYW%" goto :try_console

REM pythonw = GUI subsystem: start it detached so this console window goes away
start "" "%PYW%" "%SCRIPT%" --os windows %*
exit /b 0

:try_console
if not exist "%PY%" goto :no_python
"%PY%" "%SCRIPT%" --os windows %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" pause
endlocal & exit /b %RC%

:no_python
echo.
echo   [X] The Windows sandbox Python is missing:
echo       %PY%
echo.
echo   Either this .idf-sandbox copy has no Windows toolchain in it,
echo   or the folder is incomplete.  Copy the WHOLE project folder again,
echo   or fetch the Windows toolchain with:
echo       python .idf-sandbox\start.py prepare --download
echo.
pause
endlocal & exit /b 2

:no_script
echo.
echo   [X] start.py not found:
echo       %SCRIPT%
echo.
pause
endlocal & exit /b 2
