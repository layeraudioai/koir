@echo off
REM Interactive Song Compiler
REM Batch version of compile.sh

setlocal enabledelayedexpansion

cls
if not exist mp3 mkdir mp3
echo === MML Song Encoder Build System ===
echo.
echo Available songs:
set "SONG_COUNT=0"
for %%f in (mp3\*.mp3 midi\*.mid mml\*.mml songs\*.c) do (
    echo   [!SONG_COUNT!] %%f
    set "SONG_LIST[!SONG_COUNT!]=%%f"
    set /a SONG_COUNT+=1
)
set /a MAX_SONG_IDX=%SONG_COUNT% - 1
set /p "s_choice=Select song [0-%MAX_SONG_IDX%] (default 0): "
if "%s_choice%"=="" set "s_choice=0"
set "SELECTED_FILE=!SONG_LIST[%s_choice%]!"

echo Select Target Platform:
echo   [0] linux
echo   [1] windows
echo   [2] nintendo
set /p "p_choice=Select [0-2] (default 0): "
if "%p_choice%"=="" set "p_choice=0"
if "%p_choice%"=="0" set "PLATFORM=linux"
if "%p_choice%"=="1" set "PLATFORM=windows"
if "%p_choice%"=="2" set "PLATFORM=nintendo"

echo Select Target Architecture:
echo   [0] amd64
echo   [1] arm
echo   [2] arm64
set /p "a_choice=Select [0-2] (default 0): "
if "%a_choice%"=="" set "a_choice=0"
if "%a_choice%"=="0" set "ARCH=amd64"
if "%a_choice%"=="1" set "ARCH=arm"
if "%a_choice%"=="2" set "ARCH=arm64"

echo Scanning for available audio libraries...

REM Initialize available backends array
set "AVAILABLE_COUNT=0"
set "AVAILABLE[0]=DUMMY"

REM Check for ALSA (not typically available on Windows)
REM On Windows, we'll check for common audio backends

REM Check for SDL3 via pkg-config (if available)
where pkg-config >nul 2>nul
if %errorlevel% equ 0 (
    pkg-config --exists sdl3 >nul 2>nul
    if %errorlevel% equ 0 (
        set /a AVAILABLE_COUNT+=1
        set "AVAILABLE[!AVAILABLE_COUNT!]=SDL3"
    )
)

REM Check for SDL2 via pkg-config
where pkg-config >nul 2>nul
if %errorlevel% equ 0 (
    pkg-config --exists sdl2 >nul 2>nul
    if %errorlevel% equ 0 (
        set /a AVAILABLE_COUNT+=1
        set "AVAILABLE[!AVAILABLE_COUNT!]=SDL2"
    )
)

REM Check for SDL via sdl-config
where sdl-config >nul 2>nul
if %errorlevel% equ 0 (
    sdl-config --version >nul 2>nul
    if %errorlevel% equ 0 (
        set /a AVAILABLE_COUNT+=1
        set "AVAILABLE[!AVAILABLE_COUNT!]=SDL"
    )
)

REM Windows Multimedia API is always available on Windows
set /a AVAILABLE_COUNT+=1
set "AVAILABLE[!AVAILABLE_COUNT!]=WINMM"

REM Add DUMMY backend (already at index 0)

if %AVAILABLE_COUNT% equ 0 (
    echo No audio backends detected.
    set "SELECTED=DUMMY"
) else (
    echo Available backends for '!SELECTED_FILE!':
    for /l %%i in (0,1,%AVAILABLE_COUNT%) do (
        echo   [%%i] !AVAILABLE[%%i]!
    )
    
    set /p "choice=Select backend [0-%AVAILABLE_COUNT%]: "
    if "!choice!"=="" set "choice=0"
    
    REM Validate input
    set "valid=0"
    for /l %%i in (0,1,%AVAILABLE_COUNT%) do (
        if "%%i"=="!choice!" (
            set "valid=1"
            set "SELECTED=!AVAILABLE[%%i]!"
        )
    )
    
    if !valid! equ 0 (
        echo Invalid selection. Using default.
        set "SELECTED=!AVAILABLE[0]!"
    )
)

REM --- Automation Phase ---
for %%A in ("!SELECTED_FILE!") do set "FILE_EXT=%%~xA"
if /i "!FILE_EXT!"==".mp3" (
    echo Converting MP3 to MIDI...
    python mp32mid.py "!SELECTED_FILE!"
    for %%A in ("!SELECTED_FILE!") do set "FILE_BASE=%%~nA"
    set "SELECTED_FILE=midi\!FILE_BASE!_basic_pitch.mid"
    set "FILE_EXT=.mid"
)

if /i "!FILE_EXT!"==".mid" (
    echo Converting MIDI to MML...
    python mid2mml.py "!SELECTED_FILE!"
    for %%A in ("!SELECTED_FILE!") do set "FILE_BASE=%%~nA"
    set "SELECTED_FILE=mml\!FILE_BASE!.mml"
    set "FILE_EXT=.mml"
)

if /i "!FILE_EXT!"==".mml" (
    echo Converting MML to C...
    make tools
    for %%A in ("!SELECTED_FILE!") do set "FILE_BASE=%%~nA"
    set "SONG=songs\!FILE_BASE!_mml"
    if not exist songs mkdir songs
    mml2c "!SELECTED_FILE!" "!SONG!.c"
) else (
    for %%A in ("!SELECTED_FILE!") do set "SONG=%%~dpnA"
)
REM ------------------------

set "EXT="
if "%PLATFORM%"=="windows" set "EXT=.exe"
if "%PLATFORM%"=="nintendo" (
    if "%ARCH%"=="arm" set "EXT=.cia"
    if "%ARCH%"=="arm64" set "EXT=.nsp"
    if not "%ARCH%"=="arm" if not "%ARCH%"=="arm64" set "EXT=.nro"
)

set "backend_fn=%SELECTED%"
if /i "%SELECTED%"=="SDL3" set "backend_fn=sdl3"
if /i "%SELECTED%"=="SDL2" set "backend_fn=sdl2"
if /i "%SELECTED%"=="SDL" set "backend_fn=sdl"
if /i "%SELECTED%"=="WINMM" set "backend_fn=winmm"
if /i "%SELECTED%"=="ALSA" set "backend_fn=alsa"
if /i "%SELECTED%"=="DUMMY" set "backend_fn=dummy"

for %%A in ("!SONG!") do set "SONG_NAME=%%~nA"
set "OUTFILE=%PLATFORM%_%ARCH%_%backend_fn%_!SONG_NAME!%EXT%"

REM Handle compiler prefix (e.g. for cross-compiling)
if not "%~2"=="" (
    set "CC_CMD=%~2cc"
) else (
    if "%PLATFORM%"=="nintendo" (
        if "%ARCH%"=="arm" (set "CC_CMD=arm-none-eabi-gcc") else (set "CC_CMD=aarch64-none-elf-gcc")
    ) else (
        set "CC_CMD=gcc"
    )
)

echo Building %OUTFILE% with %SELECTED% backend...
echo --------------------------------------

REM Call make with the selected parameters
mkdir compiles
make SONG="%SONG%" AUDIO_BACKEND="%SELECTED%" CC="%CC_CMD%" PLATFORM="%PLATFORM%" ARCH="%ARCH%" OUTFILE="compiles/%OUTFILE%"

echo --------------------------------------

endlocal
