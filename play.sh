#!/bin/sh
# PRIME, X11 frontend (port/XUI.cpp): tiled map top left, Status right of
# it, Messages and Inventory below (override with PRIME_MAP/_STATUS/_MSG/
# _INV="x,y"; PRIME_VIEW = map columns shown). Saves in user/save/,
# options in user/config.txt, high scores in score/.
cd "$(dirname "$0")" || exit 1
[ -x prime ] || make -f port/Makefile -j8 >/dev/null || exit 1
mkdir -p user/save score
[ -f port/tiles.rgba ] || python3 port/mktiles.py >/dev/null || exit 1
export PRIME_TILES="$PWD/port/tiles.rgba"
exec ./prime "$@"
