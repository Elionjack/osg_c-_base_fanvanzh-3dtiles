@echo off
chcp 65001 >nul
setlocal

echo OSGB 转 Cesium 3D Tiles
echo.
set /p "INPUT_DIR=请输入 OSGB 输入目录："
if not exist "%INPUT_DIR%" (
    echo.
    echo 错误：输入目录不存在。
    pause
    exit /b 1
)

set /p "OUTPUT_DIR=请输入输出目录："
echo.
"%~dp0osgb_converter_1_1.exe" -i "%INPUT_DIR%" -o "%OUTPUT_DIR%"

echo.
echo 程序退出代码：%ERRORLEVEL%
pause

