@echo off
call "%~dp0scripts\build_demo.bat" %*
exit /b %ERRORLEVEL%
