#!/bin/sh
# Build PRIME for the browser (Emscripten + Asyncify) into web/dist;
# deploy with web/deploy.sh. Run with sh (zsh doesn't split lists).
set -e
cd "$(dirname "$0")/.."
OUT=web/dist
rm -rf "$OUT" && mkdir -p "$OUT"
[ -f port/tiles.rgba ] || python3 port/mktiles.py
SRCS=$(ls src/*.cpp | grep -v -e NEUI -e NCUI -e CrtUI)
# port/XUI.cpp's __EMSCRIPTEN__ part hands the panes to web/prime.js
em++ -O2 $EMFLAGS -std=gnu++98 -w -DNDEBUG -DNOGUI -DPRIME_X11 -Isrc -Iport \
	$SRCS src/gen/Lore.cpp port/XUI.cpp \
	-o "$OUT/prime-core.js" \
	-sASYNCIFY -sASYNCIFY_STACK_SIZE=131072 -sSTACK_SIZE=2097152 \
	-sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=64MB \
	-sEXPORTED_FUNCTIONS=_main \
	-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,HEAPU8,addRunDependency,removeRunDependency \
	-sFORCE_FILESYSTEM -lidbfs.js -sENVIRONMENT=web \
	--preload-file data@/prime/data \
	--preload-file user/keymap@/prime/keymap \
	--preload-file port/tiles.rgba@/prime/port/tiles.rgba
cp web/index.html "$HOME/Games/rvip-tools/web/rvip-wm.js" web/prime.js "$OUT/"
python3 web/make-help.py > "$OUT/help.html"
ls -la "$OUT"
