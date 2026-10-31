@echo off
title Pro DJ Link Real-Time Traffic Monitor
chcp 65001 >nul
mode con: cols=92 lines=28
cd /d "%~dp0"

set "PY_EXE=C:\Users\UNXCHR\AppData\Local\Python\bin\python.exe"

if exist "%PY_EXE%" (
    "%PY_EXE%" prodjlink_monitor.py --probe %*
) else (
    py prodjlink_monitor.py --probe %*
)

pause
