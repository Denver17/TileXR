@echo off
python "%~dp0tssh.py" %*
exit /b %ERRORLEVEL%
