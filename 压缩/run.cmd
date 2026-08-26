@echo off
setlocal
node "%~dp0compress-glb.mjs" %*
exit /b %errorlevel%
