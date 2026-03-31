#!/bin/bash
# Interactive Song Compiler
clear
echo "=== MML Song Encoder Build System ==="

if [[ "${1}" != " " ]]; 
then 
SONGS=();
idx=0;
shopt -s nullglob;
SONGS+=("${1}");
shopt -u nullglob;
SELECTED_FILE="${SONGS[0]}";
else
echo "Available songs:";
SONGS=();
idx=0;
shopt -s nullglob;
for f in mp3/*.mp3 midi/*.mid mml/*.mml songs/*.c; do
    SONGS+=("$f");
    echo "  [$idx] $f";
    ((idx++));
done;
shopt -u nullglob;
read -p "Select song [0-$((idx-1))] (default 0): " s_idx;
s_idx=${s_idx:-0};
SELECTED_FILE="${SONGS[$s_idx]}";
fi;

if [[ "${2}" != " " ]]; 
then 
p_idx=${p_idx:-0}
PLATFORMS=("linux" "windows" "nintendo")
PLATFORM=${PLATFORMS[$2]}
else
echo "Select Target Platform:"
echo "  [0] linux"
echo "  [1] windows"
echo "  [2] nintendo"
read -p "Select [0-2] (default 0): " p_idx
p_idx=${p_idx:-0}
PLATFORMS=("linux" "windows" "nintendo")
PLATFORM=${PLATFORMS[$p_idx]}
fi;

if [[ "${3}" != " " ]]; 
then
a_idx=${a_idx:-0}
ARCHS=("amd64" "arm" "arm64")
ARCH=${ARCHS[$3]}
else
echo "Select Target Architecture:"
echo "  [0] amd64"
echo "  [1] arm"
echo "  [2] arm64"
read -p "Select [0-2] (default 0): " a_idx
a_idx=${a_idx:-0}
ARCHS=("amd64" "arm" "arm64")
ARCH=${ARCHS[$a_idx]}
fi;

if [[ "${4}" != " " ]]; 
then 
echo "Scanning for available audio libraries..."
AVAILABLE=()
if pkg-config --exists alsa 2>/dev/null; then AVAILABLE+=("ALSA"); fi
if pkg-config --exists sdl3 2>/dev/null; then AVAILABLE+=("SDL3"); fi
if pkg-config --exists sdl2 2>/dev/null; then AVAILABLE+=("SDL2"); fi
if sdl-config --version &>/dev/null; then AVAILABLE+=("SDL"); fi
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OS" == "Windows_NT" ]]; then AVAILABLE+=("WINMM"); fi
AVAILABLE+=("DUMMY")

if [ ${#AVAILABLE[@]} -eq 0 ]; then
    echo "No audio backends detected."
    SELECTED="DUMMY"
else
    echo "Available backends for '$SELECTED_FILE':"
    for i in "${!AVAILABLE[@]}"; do
        echo "  [$i] ${AVAILABLE[$i]}"
    done
    SELECTED=${AVAILABLE[$4]}
fi
else
echo "Scanning for available audio libraries..."
AVAILABLE=()
if pkg-config --exists alsa 2>/dev/null; then AVAILABLE+=("ALSA"); fi
if pkg-config --exists sdl3 2>/dev/null; then AVAILABLE+=("SDL3"); fi
if pkg-config --exists sdl2 2>/dev/null; then AVAILABLE+=("SDL2"); fi
if sdl-config --version &>/dev/null; then AVAILABLE+=("SDL"); fi
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OS" == "Windows_NT" ]]; then AVAILABLE+=("WINMM"); fi
AVAILABLE+=("DUMMY")

if [ ${#AVAILABLE[@]} -eq 0 ]; then
    echo "No audio backends detected."
    SELECTED="DUMMY"
else
    echo "Available backends for '$SELECTED_FILE':"
    for i in "${!AVAILABLE[@]}"; do
        echo "  [$i] ${AVAILABLE[$i]}"
    done
    read -p "Select backend [0-$((${#AVAILABLE[@]}-1))]: " choice
    choice=${choice:-0}
    SELECTED=${AVAILABLE[$choice]}
fi
fi;
# --- Automation Phase ---
if [[ "$SELECTED_FILE" == *.mp3 ]]; then    
    echo Converting MP3 to MIDI...
    newname="${SELECTED_FILE// /_}"
    newname="${newname//\'/}"
    newname="${newname//(/}"
    newname="${newname//)/}"
    cp "$SELECTED_FILE" "${newname}"
    python mp32mid.py ${newname}
    SELECTED_FILE="${newname//.*}.mid"
fi
if [[ "$SELECTED_FILE" == *.mid ]]; then
    echo "Converting MIDI to MML..."
    newname="${SELECTED_FILE// /_}"
    newname="${newname//\'/}"
    newname="${newname//(/}"
    newname="${newname//)/}"
    cp "$SELECTED_FILE" "${newname}"

    python3 src/mid2mml.py "${newname}" || exit 1
    base=$(basename "${newname}" .mid)
    SELECTED_FILE="mml/${base}.mml"
fi

if [[ "$SELECTED_FILE" == *.mml ]]; then
    echo "Converting MML to C..."
    cd src
    make tools
    base=$(basename "$SELECTED_FILE" .mml)
    SONG="songs/${base// /_}_mml"
    mkdir -p ../songs
    cd ..
    ./mml2c "$SELECTED_FILE" "${SONG// /_}.c" || exit 1
else
    SONG="${SELECTED_FILE%.*}"
fi
# ------------------------

EXT=""
if [[ "$PLATFORM" == "windows" ]]; then EXT=".exe"; fi
if [[ "$PLATFORM" == "nintendo" ]]; then
    if [[ "$ARCH" == "arm" ]]; then EXT=".cia";
    elif [[ "$ARCH" == "arm64" ]]; then EXT=".nsp";
    else EXT=".nro"; fi
fi
backend_lower=$(echo "$SELECTED" | tr '[:upper:]' '[:lower:]' 2>/dev/null || echo "$SELECTED")
SONG_NAME=$(basename "${SONG// /_}")
OUTFILE="${PLATFORM}_${ARCH}_${backend_lower}_${SONG_NAME}${EXT}"

# Handle compiler prefix (e.g. for cross-compiling)
if [ ! -z "$5" ]; then
    CC_CMD="${5}cc"
elif [[ "$PLATFORM" == "nintendo" ]]; then
    if [[ "$ARCH" == "arm" ]]; then CC_CMD="arm-none-eabi-gcc"
    else CC_CMD="aarch64-none-elf-gcc"; fi
else
    CC_CMD="gcc"
fi

echo "Building $OUTFILE with $SELECTED backend..."
mkdir compiles
cd src
make SONG="${SONG_NAME// /_}" AUDIO_BACKEND="$SELECTED" CC="$CC_CMD" PLATFORM="$PLATFORM" ARCH="$ARCH" OUTFILE=../compiles/"$OUTFILE"
cd ..
echo "--------------------------------------"
