# PRIME 2.5a — RVIP import (2026-09-25)

Case O (C++, own UI layer `shInterface` with NCurses and NotEye frontends).
Source: https://github.com/Larzid/PRIME (shallow clone, upstream 4404414);
`git log`: upstream, then the port.

- Build: `make -f port/Makefile -j8` → `./prime` (objects in `port/obj`).
  Needs `brew install libsigsegv`; XQuartz for X11/Xft. Data tables:
  `make -f port/Makefile gen` (m4 + `tablemk`, built from `src/tablemk` with
  bison/flex/fpc) and `lore` (encyclopedia). The committed `obj/*.o` and
  `bin/tablemk` are upstream's Linux leftovers, unused.
- Run: `./play.sh` or `~/Desktop/Games/Roguelikes/PRIME.app`. Saves in
  `user/save/<name>.sav` (deleted on load), options + keymap in
  `user/config.txt`, scores in `score/`. Docs: `Docs/prime.html`.
- Frontend `port/XUI.cpp` replaces NotEye: text windows kept as cell grids,
  the map drawn from the same per-cell tile stacks as `src/NEUI.cpp`
  (`terr2tile`/`feat2tile`/… copied). Panes: Map (32×32 tiles, 40 of 64
  columns, scrolls with the hero or the targeting cursor), Status (kSide),
  Messages (history + kLog), Inventory (from the pack), pop-up = open
  kTemp/kMenu/kMenuHelp windows, each cut to its content and stacked.
  `PRIME_DUMP=<file>` writes all panes as text; `PRIME_TILEGAPS=<file>` lists
  glyphs whose tile is empty or a coloured letter.
- Tiles: `gfx/primetiles.png` (the NotEye release's sheet) + extra row 110
  from `gfx/rltiles.png` (`port/mktiles.py` → `port/tiles.rgba`, made by
  play.sh). Only the ghoul and alien embryo lacked sprites (RLTiles: grey
  hunched figure, pale larva, user asked for non-futuristic fills only).
  Webs / trap doors / portable holes use the sheet's own web and hole
  instead of `^`. Items without an own tile show their unidentified look
  plus the mini-icon overlay, as in NotEye.
- `src/Rvip.cpp`: explore `X`, `<`/`>` stair walk, Enter menu (grouped like
  `?` help), inventory with cursor + item menus (actions go through
  `objectVerbCommand()`), hooked via `rvipCommand()` in
  `shCreature::playerControl` and `listInventory()`. `shMenu` got a cursor
  (8/2, 5/Enter, click) — covers every item prompt. Return is key 13 and
  always the menu (laptop keymap's Ctrl+J shoot-south stays 10; ADOM keymap
  history moved ^M → ^P). X and Enter are bound after any keymap loads.
- Upstream bugs fixed (ASan): `makePluralNH()` returned a stack buffer and
  compared `spot-4` for 4-letter words; redraws before the hero is placed
  (NEUI did the same, crashed on arm64 in `isInShop(-10,…)`).
  `GetBuf()` ring is saved/restored around the frontend's `inv()` calls.
- Tested (ASan clean): new game, explore incl. doors/items/locked doors/
  monsters, `>` walk, Enter menu, inventory examine/wield via cursor, item
  prompt cursor, clicks, help, save → restore (status + pack identical),
  quit. Not done: web port (7) and so sound (6b).
