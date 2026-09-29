/*
 * PRIME in the browser: draws the panes of port/XUI.cpp (its __EMSCRIPTEN__
 * part calls Module.pr), keyboard and mouse input, tiling windows, saves in
 * IndexedDB. Loaded before prime-core.js. Copied from ~/Games/larn/web.
 * The map is the only canvas; the text windows and the pop-up are HTML
 * lines from the game (RVIP W0 rule 6), list icons CSS sprites of tiles.png.
 */
(function () {
	'use strict';

	var P_MAP = 0, P_STATUS = 1, P_MSG = 2, P_INV = 3, P_POP = 4;
	var WIN = ['map', 'stat', 'msg', 'inv'];          /* pane -> window id */
	var ROOT = '/prime';                            /* the game's cwd: data/, user/, score/ */
	var DIR = ROOT + '/user';                       /* IDBFS mount: save/, config.txt, score/, layout */
	var SAVE, NAME,                                 /* save/<hero name>.sav (PRIME names it) */
		 LAYOUT_FILE = DIR + '/web-layout.json';
	var TS = 32;                                    /* map cells arrive as 32x32 RGBA */
	var MAP_COLS = 64, MAP_ROWS = 20, SIDE_COLS = 40;
	var SHEET_COLS = 41, SHEET_ROWS = 111;          /* tiles.png (port/mktiles.py), 32 px tiles */
	var CURSOR = '#ffff5a';                         /* targeting cursor */
	var FONT = '"DejaVu Sans Mono", Menlo, Consolas, "Liberation Mono", monospace';
	var GUT = 6, TITLE_H = 20, BORDER = 2;
	/* tile height in px (cells are half as wide); a bigger map scrolls */
	var TILE_STEPS = [16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96];
	/* the key codes of port/XUI.cpp (the keymaps name them "up arrow", ...) */
	var KEY = { ArrowUp: 0x101, ArrowDown: 0x102, ArrowLeft: 0x103, ArrowRight: 0x104,
		Home: 0x105, End: 0x106, PageUp: 0x107, PageDown: 0x108, Insert: 0x10A, Delete: 0x10B };

	var panes = [];            /* the map: {cv, ctx, cols, rows, cw, ch, w, h, box} */
	var cells = [];            /* the map's composed cells (ImageData, 32 px) */
	var events = [];
	var app, saveReq = false;
	var cur = { y: -1, x: -1 };
	var L = null, rects = {};

	function $(id) { return document.getElementById(id); }
	function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

	/* ---------- panes ---------- */

	/* the text font: a face from the index page's fonts/ (L.face), all windows
	 * but the map (always tiles) */
	function face() { return L && L.face ? '"' + L.face + '", ' + FONT : FONT; }

	/* The map canvas holds the cells at 32 px; the zoom (L.tile) is its CSS size */
	function shape() {
		var T = panes[P_MAP];
		T.cw = T.ch = L.tile;
		T.w = T.cols * T.cw; T.h = T.rows * T.ch;
		T.cv.style.width = T.w + 'px'; T.cv.style.height = T.h + 'px';
		fit();
	}

	function makeMap(cols, rows) {
		var cv = document.querySelector('#t-map canvas');
		cv.width = cols * TS; cv.height = rows * TS;
		panes[P_MAP] = { cv: cv, ctx: cv.getContext('2d'), cols: cols, rows: rows };
		panes[P_MAP].ctx.fillRect(0, 0, cv.width, cv.height);
		shape();
	}

	function draw(y, x) {
		var T = panes[P_MAP], d = cells[y * MAP_COLS + x];
		if (!T || !d) return;
		T.ctx.putImageData(d, x * TS, y * TS);
		if (cur.y === y && cur.x === x) {           /* targeting cursor */
			T.ctx.strokeStyle = CURSOR; T.ctx.lineWidth = 2;
			T.ctx.strokeRect(x * TS + 1, y * TS + 1, TS - 2, TS - 2);
		}
	}

	/* ---------- text windows (RVIP W0 rule 6): HTML lines from the game ----------
	 * Each changed row comes trimmed, colour runs "\x05[*]#fg[/#bg]" .. "\x06",
	 * with its icon (inventory), plus the rows in use (port/XUI.cpp web_pane);
	 * the WM sets the text size (A−/A+ per window), the pop-up has the Messages size. */
	var txt = [];              /* pane -> {el, lines, tiles, n} */
	function textPane(p) {
		var el = p === P_POP ? $('pop').firstElementChild : document.querySelector('#t-' + WIN[p] + ' .body' + (p === P_INV ? '' : ' pre'));
		el.textContent = '';
		txt[p] = { el: el, lines: [], tiles: [], n: 0 };
	}
	function esc(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;'); }
	var TOK = /\x05\*?#[0-9a-f]{6}(?:\/#[0-9a-f]{6})?|[\x01\x02\x06]|[^\x01\x02\x05\x06]+/g;
	function rowHtml(s) {
		var out = '';
		(s.match(TOK) || []).forEach(function (a) {
			if (a === '\x01') out += '<span class="so">';
			else if (a === '\x02' || a === '\x06') out += '</span>';
			else if (a[0] === '\x05') { var b = a[1] === '*', c = a.slice(b ? 2 : 1).split('/'); out += '<span style="color:' + c[0] + (b ? ';font-weight:bold' : '') + (c[1] ? ';background:' + c[1] : '') + '">'; }
			else out += esc(a);
		});
		return out;
	}
	function drawRow(p, y) {
		var T = txt[p], d = T && T.el.children[y];
		if (!d) return;
		d.innerHTML = rowHtml(T.lines[y] || '');
		var ic = T.tiles[y] >= 0 && icon(T.tiles[y]);
		if (ic) d.insertBefore(ic, d.firstChild);
	}
	function setRows(p, n) {
		var T = txt[p];
		while (T.el.children.length < n) { T.el.appendChild(document.createElement('div')); drawRow(p, T.el.children.length - 1); }
		while (T.el.children.length > n) T.el.removeChild(T.el.lastChild);
		T.n = n;
	}
	function msgMark() {                          /* before a Messages change: was it at the end? */
		var b = txt[P_MSG] && txt[P_MSG].el.parentNode;
		if (b && pr.follow == null) pr.follow = b.scrollTop + b.clientHeight >= b.scrollHeight - 4;
	}
	function popFont() { $('pop').style.fontSize = RvipWM.fontSize('msg') + 'px'; placePop(); }   /* pop-ups use the Messages size */
	function placePop() { if (!$('pop').hidden && rects.map) RvipWM.popup($('pop'), { x: L.tile / 2 }); }
	/* the top-bar font on every text window and the pop-up */
	function applyFace() {
		['#t-stat .body', '#t-msg .body', '#t-inv .body', '#t-vis .body', '#pop'].forEach(function (q) {
			var e = document.querySelector(q); if (e) e.style.fontFamily = face(); });
	}

	/* List icons (Inventory, Visible): the game's tile stack for an item or
	 * monster as layers "tx,ty[,#colour];..." (a colour multiplies that layer,
	 * masked to the tile) of tiles.png, CSS sprites sized in em (A+ grows them) */
	var icons = {};            /* key -> [[tx, ty, colour]] */
	var IC = 1.25;             /* icon side in em */
	function sprite(tx, ty) {
		return 'url(tiles.png) ' + -tx * IC + 'em ' + -ty * IC + 'em / ' + SHEET_COLS * IC + 'em ' + SHEET_ROWS * IC + 'em no-repeat';
	}
	function icon(key) {
		var ls = icons[key];
		if (!ls) return null;
		var e = document.createElement('span');
		e.className = 'ic';
		ls.forEach(function (l) {
			var s = document.createElement('span'), sp = sprite(l[0], l[1]);
			if (l[2]) {
				s.style.background = sp + ', ' + l[2];
				s.style.backgroundBlendMode = 'multiply';
				s.style.webkitMask = s.style.mask = sp;
			} else s.style.background = sp;
			e.appendChild(s);
		});
		return e;
	}

	/* ---------- tiling layout ---------- */
	/*
	 *   +-----------------------+-----------+   side:   x of the left | right column
	 *   |          map          |  status   |   bottom: y of map | messages
	 *   |                       +-----------+   stat:   y of status | inventory
	 *   +-----------------------+ inventory |
	 *   |       messages        |           |
	 *   +-----------------------+-----------+
	 */
	var SPLITS = ['bottom', 'stat', 'side'];

	function areaSize() {
		var g = $('game');
		return { w: g.clientWidth, h: g.clientHeight };
	}

	function defaultLayout() {
		var A = areaSize(), W = A.w, H = A.h;
		if (W < 400 || H < 300) { W = 1280; H = 720; }
		var font = 13, tile = TILE_STEPS[0];
		var sideW = SIDE_COLS * Math.ceil(0.62 * font) + BORDER + 4;   /* 13 px text, roughly */
		/* the map may scroll sideways: size the tiles by its height */
		TILE_STEPS.forEach(function (t) { if (MAP_ROWS * t + BORDER <= H * 0.72 && t <= 48) tile = t; });
		var mapH = MAP_ROWS * tile + BORDER;
		return { v: 1, tile: tile, auto: true,
			split: { bottom: (mapH + GUT / 2) / H, side: (W - sideW - GUT / 2) / W,
				stat: (20 * Math.round(font * 1.3) + TITLE_H + BORDER + GUT / 2) / H } };
	}

	function loadLayout() {
		var d = defaultLayout();
		try {
			var s = JSON.parse(Module.FS.readFile(LAYOUT_FILE, { encoding: 'utf8' }));
			if (s && s.v === 1) {
				if (!s.auto) {
					d.auto = false;
					SPLITS.forEach(function (k) { if (s.split[k] > 0 && s.split[k] < 1) d.split[k] = s.split[k]; });
					if (TILE_STEPS.indexOf(s.tile) >= 0) d.tile = s.tile;
				}
				if (s.wm) d.wm = s.wm;
				if (s.font && d.wm && !d.wm.fs) d.wm.fs = { msg: s.font.msg, stat: s.font.stat, inv: s.font.inv, vis: s.font.vis };   /* old layout: sizes were ours */
				if (typeof s.face === 'string') d.face = s.face;
			}
		} catch (err) { /* nothing saved yet */ }
		L = d;
		$('sel-font').value = L.face || '';   /* if the font list came first */
		loadFace(L.face);
	}

	var saveTimer = 0;
	function saveLayout() {
		clearTimeout(saveTimer);
		saveTimer = setTimeout(function () {
			try { Module.FS.writeFile(LAYOUT_FILE, JSON.stringify(L)); app.sync(); }
			catch (err) { console.warn('layout not saved', err); }
		}, 400);
	}

	function place(el, r) {
		el.style.left = r[0] + 'px'; el.style.top = r[1] + 'px';
		el.style.width = Math.max(0, r[2]) + 'px'; el.style.height = Math.max(0, r[3]) + 'px';
	}

	/* the map never shrinks: bigger than its window, it scrolls with the hero */
	function fit() {
		var T = panes[P_MAP], r = rects.map;
		if (!T || !r) return;
		T.box = { w: r[2] - BORDER, h: r[3] - BORDER - ($('game').classList.contains('wm-single') ? 0 : TITLE_H) };
		scrollMap(true);
	}

	var hero = { y: -1, x: -1 }, off = { x: 0, y: 0 };
	/* Keep the hero in the middle half of the map window; recentre when it
	 * leaves it (or always, after a zoom, resize or new level) */
	function scrollMap() {
		var T = panes[P_MAP];
		if (!T || !T.box || hero.x < 0) return;
		T.cv.style.width = T.w + 'px'; T.cv.style.height = T.h + 'px';
		off = RvipWM.center(T.cv, (hero.x + 0.5) * T.cw, (hero.y + 0.5) * T.ch, T.w, T.h, T.box.w, T.box.h);
	}

	var wm = null;
	function applyDom() { if (wm) wm.apply(); }
	function makeWM() {
		var s = defaultLayout().split, A = areaSize();
		var line = Math.round(13 * 1.3) + 4, stat = Math.round(13 * 1.3) + 4;
		wm = RvipWM({
			area: $('game'), menu: $('btn-layout'),
			wins: [{ id: 'map', title: 'Map' }, { id: 'msg', title: 'Messages' }, { id: 'stat', title: 'Status' }, { id: 'inv', title: 'Inventory' }, { id: 'vis', title: 'Visible' }],
			multi: { d: 'h', r: s.side, a: { d: 'v', r: s.bottom, a: 'map', b: 'msg' }, b: { d: 'v', r: s.stat, a: 'stat', b: { d: 'v', r: 0.6, a: 'inv', b: 'vis' } } },
			single: { d: 'v', r: line / A.h, a: 'msg', b: { d: 'v', r: 1 - stat / (A.h - line), a: 'map', b: 'stat' } },
			state: L.wm,
			save: function (st) { L.wm = st; saveLayout(); },
			layout: function (r) { rects = r; fit(); placePop(); var mb = txt[P_MSG] && txt[P_MSG].el.parentNode; if (mb) mb.scrollTop = mb.scrollHeight; },
			/* A- / A+: the map steps its tiles; the text windows are the WM's; the pop-up follows Messages */
			zoom: { map: function (px, d) { zoomMap(d); }, msg: popFont },
			onReset: resetLayout
		});
		wm.apply();
	}

	function zoomMap(d) {
		var i = clamp(TILE_STEPS.indexOf(L.tile) + d, 0, TILE_STEPS.length - 1);
		L.tile = TILE_STEPS[i]; L.auto = false;
		shape(); applyDom(); saveLayout();
		app.status('Map tiles: ' + L.tile + ' px');
		setTimeout(function () { app.status(''); }, 1200);
	}

	function resetLayout() {
		var a = L.audio, fc = L.face;
		L = defaultLayout(); L.audio = a; L.face = fc; L.wm = wm.state();
		if (panes[P_MAP]) shape();
		popFont(); applyDom(); saveLayout();
	}

	/* ---------- called by the game (port/XUI.cpp) ---------- */

	var pr = {
		init: function (p, cols, rows) {
			if (!L) loadLayout();
			if (p === P_MAP) makeMap(cols, rows); else textPane(p);
			if (p === P_INV) { $('game').hidden = false; makeWM(); applyFace(); popFont(); }
		},
		tile: function (x, y, ptr) {             /* one composed map cell */
			cells[y * MAP_COLS + x] = new ImageData(new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, TS * TS * 4).slice(), TS, TS);
			draw(y, x);
		},
		line: function (p, y, s, c, t) {
			var T = txt[p];
			if (!T) return;
			if (p === P_MSG) msgMark();
			T.lines[y] = s; T.tiles[y] = t;
			if (y < T.n) drawRow(p, y);
		},
		rows: function (p, n) { if (!txt[p]) return; if (p === P_MSG) msgMark(); setRows(p, n); },
		popup: function (rows, cols) {
			if (!rows) { $('pop').hidden = true; return; }
			textPane(P_POP);
			$('pop').hidden = false;
		},
		flush: function (fy, fx, cy, cx) {
			var old = { y: cur.y, x: cur.x };
			cur.y = cy; cur.x = cx;                  /* targeting cursor, -1: none */
			if (old.y >= 0) draw(old.y, old.x);
			if (cy >= 0) draw(cy, cx);
			if (fy >= 0 && (fy !== hero.y || fx !== hero.x)) { var first = hero.x < 0; hero.y = fy; hero.x = fx; scrollMap(first); }
			var mb = txt[P_MSG] && txt[P_MSG].el.parentNode;   /* follow the newest message unless scrolled up */
			if (mb && pr.follow) mb.scrollTop = mb.scrollHeight;
			pr.follow = null;
			placePop();
		},
		vis: function (s) { RvipWM.visible(document.querySelector('#t-vis .body'), s, icon); },
		icon: function (key, s) {
			icons[key] = s.split(';').filter(Boolean).map(function (l) { var f = l.split(','); return [+f[0], +f[1], f[2] || '']; });
		},
		key: function (atCmd) { RvipWM.prompt.wait(atCmd); return events.length ? events.shift() : -1; },
		prompt: function (s) { RvipWM.prompt.text(s); },
		pending: function () { return events.length ? 1 : 0; },
		requestSave: function () { saveReq = true; },   /* also for testing */
		wantSave: function () {
			if (!saveReq || !app.running) return 0;
			saveReq = false;
			return 1;
		},
		saved: function () { app.sync(); },
		end: function (saved) {
			app.running = false;
			app.sync(function () {
				$('overlay-msg').textContent = saved ? 'Your game has been saved. Play again to continue it.'
					: 'The game is over. Play again for a new character.';
				$('overlay').hidden = false;
			});
		}
	};

	/* ---------- input ---------- */
	function onKey(e) {
		if (!app.running || e.isComposing || e.metaKey) return;
		var k = e.key, code = e.code || '', m = /^Numpad(\d)$/.exec(code), c, f = /^F(\d+)$/.exec(k);
		if (m) c = 48 + +m[1];                       /* keypad: digits (RVIP numpad rules) */
		else if (code === 'NumpadEnter' || k === 'Enter') c = 13;
		else if (code === 'NumpadDecimal') c = 46;
		else if (f && +f[1] <= 12) c = 0x110 + +f[1];
		else if (k === 'Escape') c = 27;
		else if (k === 'Backspace') c = 8;
		else if (k === 'Tab') c = 9;
		else if (KEY[k]) c = KEY[k];
		else if (k.length === 1) {
			c = k.charCodeAt(0);
			if (e.ctrlKey && !e.altKey) {
				var u = k.toUpperCase().charCodeAt(0);
				if (u >= 64 && u <= 95) c = u & 0x1F;
			}
			if (c > 255) return;
		}
		else return;
		events.push(c);
		e.preventDefault();
	}

	/* ---------- saves: IndexedDB (IDBFS) ---------- */
	function hasSave() { try { Module.FS.stat(SAVE); return true; } catch (e) { return false; } }
	/* a click on a pop-up row: the game maps the row to a menu entry */
	function onPopClick(e) {
		var T = txt[P_POP], d = e.target.closest && e.target.closest('#pop .txt > div');
		if (!T || !d || !app.running) return;
		var row = Array.prototype.indexOf.call(T.el.children, d);
		if (row >= 0) events.push(0x10000 + row);
	}
	function putSave(file, data) { Module.FS.writeFile(SAVE, data); }
	function clearSave() { try { Module.FS.unlink(SAVE); } catch (e) { } }

	/* ---------- fonts ---------- */
	function loadFace(n, now) {
		var redraw = function () {
			applyFace(); applyDom();
		};
		if (!n) { if (now) redraw(); return; }
		var ff = new FontFace(n, 'url(../fonts/' + n + '.woff)');
		ff.load().then(function () { document.fonts.add(ff); redraw(); }).catch(function () { app.status('Could not load the font ' + n + '.', true); });
	}

	/* ---------- startup ---------- */
	app = RvipApp({ name: 'prime', save: function () { return hasSave() ? SAVE : null; }, clear: clearSave, put: putSave,
		exportName: function () { return 'prime.sav'; },
		flush: function (done) { saveReq = true; setTimeout(done, 1500); } });   /* the game saves at its next wantSave() poll */
	/* the player's name: asked once, kept in this game's IndexedDB folder (never localStorage) */
	function askName(max, bad) {
		var FS = Module.FS, f = DIR + '/web-name', n = '';
		try { n = FS.readFile(f, { encoding: 'utf8' }); } catch (e) { }
		if (!n) { n = (prompt('What is your name, adventurer?', '') || '').replace(bad, '').trim().slice(0, max); if (n) { FS.writeFile(f, n); app.sync(); } }
		return n;
	}
	window.Module = {
		pr: pr,
		preRun: [function () {
			var FS = Module.FS;
			FS.mkdirTree(DIR);
			FS.mount(Module.IDBFS, {}, DIR);
			FS.chdir(ROOT);                      /* the game's relative data/, user/, score/ */
			Module.addRunDependency('idbfs');
			FS.syncfs(true, function (err) {
				if (err) app.status('Could not read saved games from IndexedDB (' + err + '). Saving may not work in this browser mode.', true);
				['save', 'score'].forEach(function (d) { try { FS.mkdir(DIR + '/' + d); } catch (e) { } });
				try { FS.symlink(DIR + '/score', ROOT + '/score'); } catch (e) { }
				/* hero name = -u NAME (nameOK: <=14 printable, no slashes); a pre-name save/player.sav plays out first */
				NAME = 'player';
				try { FS.stat(DIR + '/save/player.sav'); } catch (e) {
					NAME = askName(14, /[^ -~]|[\/\\]/g);
					if (!NAME) NAME = 'player';
				}
				SAVE = DIR + '/save/' + NAME + '.sav';
				Module.arguments.push('-u', NAME);   /* same array the runtime captured */
				/* the keymaps come with the game, not from storage */
				try { FS.unlink(DIR + '/keymap'); } catch (e) { }
				FS.symlink(ROOT + '/keymap', DIR + '/keymap');
				Module.removeRunDependency('idbfs');
			});
		}],
		arguments: [],
		onRuntimeInitialized: function () {
			app.running = true;
			saveReq = true;                      /* PRIME deleted the save it loaded: write it back */
			app.status('');
		},
		print: function (s) { console.log(s); },
		printErr: function (s) { console.warn(s); },
		setStatus: function (s) { if (s && !app.running) app.status(s.replace(/\(\d+\/\d+\)/, '').trim() || 'Loading…'); },
		onAbort: function (what) { app.crashed(what); }
	};

	/* autosave: every 2 minutes and when the page is hidden */
	setInterval(function () { saveReq = true; }, 120000);
	document.addEventListener('visibilitychange', function () { if (document.hidden) { saveReq = true; app.sync(); } });
	window.addEventListener('pagehide', function () { app.sync(); });
	window.addEventListener('beforeunload', function (e) { if (app.running) { e.preventDefault(); e.returnValue = ''; } });

	document.addEventListener('keydown', onKey);
	document.addEventListener('DOMContentLoaded', function () {
		RvipWM.dropdown($('btn-file'), $('menu-file'));
		RvipWM.fonts.then(function (list) {
			var sel = $('sel-font');
			RvipWM.fontOptions(sel);
			sel.value = (L && L.face) || '';
		}).catch(function () { });
		$('sel-font').onchange = function () { if (!L) return; L.face = this.value; saveLayout(); loadFace(this.value, true); this.blur(); };
		$('btn-restart').onclick = function () { location.reload(); };
		$('pop').addEventListener('mousedown', onPopClick);
		document.querySelectorAll('button').forEach(function (b) {
			b.addEventListener('mousedown', function (e) { e.preventDefault(); });
		});
	});
	var resizeTimer = 0;
	window.addEventListener('resize', function () {
		if (!L) return;
		clearTimeout(resizeTimer);
		resizeTimer = setTimeout(function () {
			if (L.auto) {                        /* not customised: follow the window */
				var d = defaultLayout();
				if (d.tile !== L.tile) { L.tile = d.tile; if (panes[P_MAP]) shape(); }
			}
			applyDom();
		}, 150);
	});
})();
