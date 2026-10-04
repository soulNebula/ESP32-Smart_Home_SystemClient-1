@echo off
REM ===========================================================================
REM  monitor.cmd -- 双击即可运行的串口监视器包装
REM  真正的实现在 monitor.ps1；本机没有 pwsh(PowerShell 7)，必须用 powershell 5.1
REM ===========================================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor.ps1" %*
