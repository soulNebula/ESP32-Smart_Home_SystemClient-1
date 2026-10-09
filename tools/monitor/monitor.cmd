@echo off
REM ===========================================================================
REM  monitor.cmd -- serial monitor (legacy entry point, kept for convenience)
REM
REM  It just calls the sandbox entry: start.py monitor
REM  Modern way:  python .idf-sandbox\start.py monitor
REM ===========================================================================
setlocal EnableExtensions
chcp 65001 >nul 2>&1
cd /d "%~dp0..\.."

set "SBPY=%~dp0..\..\.idf-sandbox\python\python.exe"
set "SCRIPT=%~dp0..\..\.idf-sandbox\start.py"

if not exist "%SBPY%" goto :no_python
if not exist "%SCRIPT%" goto :no_script

"%SBPY%" "%SCRIPT%" monitor --no-pause %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" pause
endlocal & exit /b %RC%

:no_python
echo.
echo   [X] Sandbox Python not found:
echo       %SBPY%
echo.
echo   Please copy the WHOLE project folder (including .idf-sandbox).
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
