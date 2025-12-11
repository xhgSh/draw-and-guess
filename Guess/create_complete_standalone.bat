@echo off
echo 正在创建完整的独立Qt客户端...

REM 创建独立目录
if not exist "Guess_Standalone" mkdir Guess_Standalone
if not exist "Guess_Standalone\platforms" mkdir Guess_Standalone\platforms

REM 复制可执行文件
copy "build\Desktop_Qt_6_9_3_MinGW_64_bit-Debug\debug\Guess.exe" "Guess_Standalone\"

REM 复制Qt核心库文件
set QT_DIR=D:\tools\Qt\6.9.3\mingw_64\bin

copy "%QT_DIR%\Qt6Core.dll" "Guess_Standalone\"
copy "%QT_DIR%\Qt6Gui.dll" "Guess_Standalone\"
copy "%QT_DIR%\Qt6Network.dll" "Guess_Standalone\"
copy "%QT_DIR%\Qt6Widgets.dll" "Guess_Standalone\"
copy "%QT_DIR%\Qt6OpenGL.dll" "Guess_Standalone\"
copy "%QT_DIR%\Qt6OpenGLWidgets.dll" "Guess_Standalone\"

REM 复制MinGW运行时
copy "%QT_DIR%\libgcc_s_seh-1.dll" "Guess_Standalone\"
copy "%QT_DIR%\libstdc++-6.dll" "Guess_Standalone\"
copy "%QT_DIR%\libwinpthread-1.dll" "Guess_Standalone\"

REM 复制平台插件
copy "D:\tools\Qt\6.9.3\mingw_64\plugins\platforms\qwindows.dll" "Guess_Standalone\platforms\"

echo.
echo ✅ 独立客户端创建完成！
echo 📁 可执行文件位置: Guess_Standalone\Guess.exe
echo 🎮 现在可以双击运行客户端了！
echo.
pause
