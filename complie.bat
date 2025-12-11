@echo off
chcp 65001
echo ===================================
echo      Draw and Guess Builder
echo ===================================

echo [1/3] Compiling Server...
cd server
if exist draw_guess_server.exe del draw_guess_server.exe
D:\env\Cygwin\bin\gcc.exe -o draw_guess_server.exe draw_guess_server.c protocol.c sqlite3.c -lpthread
if %errorlevel% == 0 (
    echo    - Server compiled successfully.
) else (
    echo    - Failed to compile server!
    pause
    exit /b 1
)
cd ..

echo [2/3] Compiling Client (Release)...
cd Guess

REM Clean previous build if needed (optional)
if exist Makefile (
    D:\tools\Qt\Tools\mingw1310_64\bin\mingw32-make.exe clean > nul 2>&1
)

REM Run qmake to generate Makefile for Release
echo    - Running qmake...
D:\tools\Qt\6.9.3\mingw_64\bin\qmake.exe Guess.pro -spec win32-g++ "CONFIG+=release"

REM Run make to compile
echo    - Running make (this may take a while)...
D:\tools\Qt\Tools\mingw1310_64\bin\mingw32-make.exe -j8

if %errorlevel% == 0 (
    echo    - Client compiled successfully.
) else (
    echo    - Failed to compile client!
    pause
    cd ..
    exit /b 1
)

cd ..

echo [3/3] Packaging Client...
set DEST=Guess\Guess_Standalone
if not exist "%DEST%" mkdir "%DEST%"
if not exist "%DEST%\platforms" mkdir "%DEST%\platforms"

REM Copy executable (From CLI build, it usually goes to Guess/release/Guess.exe)
if exist "Guess\release\Guess.exe" (
    copy /Y "Guess\release\Guess.exe" "%DEST%\" > nul
) else (
    echo    - Error: Could not find compiled Guess.exe in Guess\release\
    pause
    exit /b 1
)

REM Copy Qt DLLs
set QT_BIN=D:\tools\Qt\6.9.3\mingw_64\bin
copy /Y "%QT_BIN%\Qt6Core.dll" "%DEST%\" > nul
copy /Y "%QT_BIN%\Qt6Gui.dll" "%DEST%\" > nul
copy /Y "%QT_BIN%\Qt6Network.dll" "%DEST%\" > nul
copy /Y "%QT_BIN%\Qt6Widgets.dll" "%DEST%\" > nul
copy /Y "%QT_BIN%\Qt6OpenGL.dll" "%DEST%\" > nul
copy /Y "%QT_BIN%\Qt6OpenGLWidgets.dll" "%DEST%\" > nul
copy /Y "%QT_BIN%\libgcc_s_seh-1.dll" "%DEST%\" > nul
copy /Y "%QT_BIN%\libstdc++-6.dll" "%DEST%\" > nul
copy /Y "%QT_BIN%\libwinpthread-1.dll" "%DEST%\" > nul

REM Copy Platforms
copy /Y "D:\tools\Qt\6.9.3\mingw_64\plugins\platforms\qwindows.dll" "%DEST%\platforms\" > nul

echo.
echo ===================================
echo      Build Complete!
echo ===================================
echo Server: server/draw_guess_server.exe
echo Client: Guess/Guess_Standalone/Guess.exe
echo.
pause
