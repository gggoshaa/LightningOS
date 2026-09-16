@echo off
setlocal
cd /d "%~dp0"
python build.py || exit /b 1
python run.py
