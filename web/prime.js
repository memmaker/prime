/*
 * PRIME in the browser: draws the panes of port/XUI.cpp (its __EMSCRIPTEN__
 * part calls Module.pr), keyboard and mouse input, tiling windows, saves in
 * IndexedDB. Loaded before prime-core.js. Copied from ~/Games/larn/web.
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
	/* shColor order, as the text colours of port/XUI.cpp */
	var PAL = ['#000', '#465ae6', '#00af00', '#00afaf', '#be1e1e', '#b432b4', '#b46e14', '#b9b9b9',
		'#6e6e6e', '#6e82ff', '#5aff5a', '#5affff', '#ff5f5f', '#ff6eff', '#ffff5a', '#fff'];
	var FONT = '"DejaVu Sans Mono", Menlo, Consolas, "Liberation Mono", monospace';
	var FG = '#dcdcdc', BG = '#000';
	var GUT = 6, TITLE_H = 20, BORDER = 2;
	/* tile height in px (cells are half as wide); a bigger map scrolls */
	var TILE_STEPS = [16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96];
	/* the key codes of port/XUI.cpp (the keymaps name them "up arrow", ...) */
	var KEY = { ArrowUp: 0x101, ArrowDown: 0x102, ArrowLeft: 0x103, ArrowRight: 0x104,
		Home: 0x105, End: 0x106, PageUp: 0x107, PageDown: 0x108, Insert: 0x10A, Delete: 0x10B };

	var panes = [];            /* {cv, ctx, cols, rows, cw, ch, pad, buf} */
	var events = [];
	var mapImg = document.createElement('canvas'), mapCtx;   /* the whole map at 32 px */
	mapImg.width = MAP_COLS * TS; mapImg.height = MAP_ROWS * TS;
	mapCtx = mapImg.getContext('2d');
	var app, saveReq = false;
	var cur = { y: -1, x: -1 };
	var dpr = Math.max(1, Math.min(3, window.devicePixelRatio || 1));
	var L = null, rects = {};

	function $(id) { return document.getElementById(id); }
	function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

	/* ---------- panes ---------- */

	/* the text font: a face from the index page's fonts/ (L.face), all panes
	 * but the map (always tiles) */
	function face() { return L && L.face ? '"' + L.face + '", ' + FONT : FONT; }
	function measure(px) {
		var c = document.createElement('canvas').getContext('2d');
		c.font = px + 'px ' + face();
		return Math.ceil(c.measureText('M').width);
	}

	/* Cell size from the zoom settings; rebuilds the canvas and redraws */
	function shape(p) {
		var T = panes[p];
		if (p === P_MAP) { T.cw = L.tile; T.ch = L.tile; T.pad = 0; }
		else {
			var f = RvipWM.fontSize(p === P_POP ? 'msg' : WIN[p]);   /* A−/A+ per window (WM state); pop-ups use the Messages size */
			T.cw = measure(f); T.ch = Math.round(f * 1.3); T.pad = p === P_POP ? T.cw : 0;
			T.font = f + 'px ' + face();
		}
		var w = T.cols * T.cw + 2 * T.pad, h = T.rows * T.ch + 2 * T.pad;
		T.cv.width = Math.round(w * dpr); T.cv.height = Math.round(h * dpr);
		T.w = w; T.h = h;
		T.ctx = T.cv.getContext('2d');
		T.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
		T.ctx.imageSmoothingEnabled = false;                 /* nearest-neighbour tiles */
		T.ctx.fillStyle = BG; T.ctx.fillRect(0, 0, w, h);
		for (var i = 0; i < T.cols * T.rows; i++) draw(p, (i / T.cols) | 0, i % T.cols);
		fit(p);
	}

	function makePane(p, cols, rows) {
		var cv = p === P_POP ? document.querySelector('#pop canvas') : document.querySelector('#t-' + WIN[p] + ' canvas');
		var n = cols * rows;
		panes[p] = { cv: cv, cols: cols, rows: rows, ch_: new Int32Array(n).fill(0x720) };
		shape(p);
	}

	function draw(p, y, x) {
		var T = panes[p], c = T.ctx, i = y * T.cols + x;
		var px = T.pad + x * T.cw, py = T.pad + y * T.ch;
		if (p === P_MAP) {
			c.drawImage(mapImg, x * TS, y * TS, TS, TS, px, py, T.cw, T.ch);
			if (cur.y === y && cur.x === x) {           /* targeting cursor */
				c.strokeStyle = PAL[14]; c.lineWidth = 1;
				c.strokeRect(px + 0.5, py + 0.5, T.cw - 1, T.ch - 1);
			}
			return;
		}
		var ic = p === P_INV ? invIcon.cover[i] : undefined;
		if (ic !== undefined) drawIcon(T, y, ic, y * T.cols + ic);   /* the icon's 3 cells */
		else textCell(T, i, px, py);
	}

	/* One text cell: char | fg << 8 | bg << 12 */
	function textCell(T, i, px, py) {
		var c = T.ctx;
		/* char | fg << 8 | bg << 12; a background colour means black text on it */
		var ch = T.ch_[i], k = ch & 0xff, fg = (ch >> 8) & 15, bg = (ch >> 12) & 15;
		c.fillStyle = PAL[bg];
		c.fillRect(px, py, T.cw, T.ch);
		if (k > 32) {
			c.font = fg >= 14 && !bg ? 'bold ' + T.font : T.font;
			c.textAlign = 'center'; c.textBaseline = 'middle';
			c.fillStyle = bg ? '#000' : PAL[fg || 7];
			c.fillText(String.fromCharCode(k), px + T.cw / 2, py + T.ch / 2 + 1);
		}
	}

	/* List icons (the game composes them, see objIcon () in port/XUI.cpp) */
	var icons = {};            /* key -> 32x32 canvas */
	var invIcon = { cover: {} }; /* Inventory: cell of col 2 -> icon key; cover: cell -> col 2 */
	/* An inventory icon: centred across cols x0..x0+2, square with side
	 * min(2*cw, ch) (keeps its aspect), each cell draws its part clipped */
	function drawIcon(T, y, x0, i0) {
		var c = T.ctx, py = T.pad + y * T.ch, key = invIcon[i0], im = icons[key];
		for (var x = x0; x < x0 + 3 && x < T.cols; x++) {
			var px = T.pad + x * T.cw;
			textCell(T, y * T.cols + x, px, py);
		}
		if (!im) return;
		var sd = Math.min(2 * T.cw, T.ch), l = T.pad + x0 * T.cw + (3 * T.cw - sd) / 2, t = py + (T.ch - sd) / 2;
		c.save(); c.beginPath(); c.rect(T.pad + x0 * T.cw, py, 3 * T.cw, T.ch); c.clip();
		c.drawImage(im, l, t, sd, sd);
		c.restore();
	}
	/* Visible window icon: the same tile, 16 px */
	function visIcon(t) {
		var im = icons[t];
		if (!im) return null;
		var e = document.createElement('canvas');
		e.width = e.height = TS; e.className = 'wm-ic';
		e.style.cssText = 'width:16px;height:16px;image-rendering:pixelated;vertical-align:middle';
		e.getContext('2d').drawImage(im, 0, 0);
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
		var sideW = SIDE_COLS * measure(font) + BORDER + 4;
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
		$('sel-font').value = L.face || '';   /* if fonts.json came first */
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

	/* Show a canvas at its size, or scaled down to fit its window (never clipped) */
	function fit(p) {
		var T = panes[p];
		if (!T) return;
		var box;
		if (p === P_POP) {
			if (!rects.map) return;
			var A = RvipWM.popupBox();
			box = { w: A.w - L.tile, h: A.h };
		} else {
			var r = rects[WIN[p]];
			if (!r) return;
			box = { w: r[2] - BORDER, h: r[3] - BORDER - ($('game').classList.contains('wm-single') ? 0 : TITLE_H) };
		}
		/* the map never shrinks: bigger than its window, it scrolls with the hero */
		if (p === P_MAP) { T.box = box; scrollMap(true); return; }
		var sc = Math.min(1, box.w / T.w, box.h / T.h);
		T.cv.style.width = T.w * sc + 'px';
		T.cv.style.height = T.h * sc + 'px';
		if (p === P_POP) RvipWM.popup($('pop'), { x: L.tile / 2 });
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
			layout: function (r) { rects = r; WIN.forEach(function (id, p) { fit(p); }); fit(P_POP); },
			zoom: { map: function (px, d) { zoomMap(d); }, msg: zoomText, stat: zoomText, inv: zoomText },   /* text panes: redraw at the WM size */
			onReset: resetLayout
		});
		wm.apply();
	}

	function zoomMap(d) {
		var i = clamp(TILE_STEPS.indexOf(L.tile) + d, 0, TILE_STEPS.length - 1);
		L.tile = TILE_STEPS[i]; L.auto = false;
		shape(P_MAP); applyDom(); saveLayout();
		app.status('Map tiles: ' + L.tile + ' px');
		setTimeout(function () { app.status(''); }, 1200);
	}

	function zoomText() {
		for (var p = 1; p < panes.length; p++) if (panes[p]) shape(p);
		applyDom();
	}

	function resetLayout() {
		var a = L.audio, fc = L.face;
		L = defaultLayout(); L.audio = a; L.face = fc; L.wm = wm.state();
		for (var p = 0; p < panes.length; p++) if (panes[p]) shape(p);
		applyDom(); saveLayout();
	}

	/* ---------- called by the game (port/XUI.cpp) ---------- */

	var pr = {
		init: function (p, cols, rows) {
			if (!L) loadLayout();
			makePane(p, cols, rows);
			if (p === P_INV) { $('game').hidden = false; makeWM(); zoomText(); }   /* reshape at the stored sizes */
		},
		put: function (p, y, x, ch) {
			var T = panes[p];
			if (!T || y < 0 || x < 0 || y >= T.rows || x >= T.cols) return;
			T.ch_[y * T.cols + x] = ch;
			draw(p, y, x);
		},
		tile: function (x, y, ptr) {             /* one composed map cell */
			var d = new ImageData(new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, TS * TS * 4).slice(), TS, TS);
			mapCtx.putImageData(d, x * TS, y * TS);
			if (panes[P_MAP]) draw(P_MAP, y, x);
		},
		popup: function (rows, cols) {
			if (!rows) { $('pop').hidden = true; panes[P_POP] = null; return; }
			makePane(P_POP, cols, rows);
			$('pop').hidden = false;
			fit(P_POP);
		},
		flush: function (fy, fx, cy, cx) {
			var old = { y: cur.y, x: cur.x };
			cur.y = cy; cur.x = cx;                  /* targeting cursor, -1: none */
			if (panes[P_MAP] && old.y >= 0) draw(P_MAP, old.y, old.x);
			if (panes[P_MAP] && cy >= 0) draw(P_MAP, cy, cx);
			if (fy >= 0 && (fy !== hero.y || fx !== hero.x)) { var first = hero.x < 0; hero.y = fy; hero.x = fx; scrollMap(first); }
		},
		vis: function (s) { RvipWM.visible(document.querySelector('#t-vis .body'), s, visIcon); },
		icon: function (key, ptr) {
			var cv = document.createElement('canvas');
			cv.width = cv.height = TS;
			cv.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, TS * TS * 4).slice(), TS, TS), 0, 0);
			icons[key] = cv;
		},
		/* "y,x,key;..." for the rows of the Inventory pane */
		invIcons: function (s) {
			var T = panes[P_INV], old = invIcon, n = { cover: {} };
			if (!T) return;
			s.split(';').forEach(function (e) {
				if (!e) return;
				var f = e.split(','), i = +f[0] * T.cols + +f[1];
				n[i] = +f[2];
				for (var k = 0; k < 3; k++) n.cover[i + k] = +f[1];
			});
			invIcon = n;
			Object.keys(old.cover).concat(Object.keys(n.cover)).forEach(function (i) {
				i = +i;
				var r = i - (i % T.cols);
				if (old.cover[i] === n.cover[i] && old[r + old.cover[i]] === n[r + n.cover[i]]) return;
				draw(P_INV, (i / T.cols) | 0, i % T.cols);
			});
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
		var T = panes[P_POP];
		if (!T || !app.running) return;
		var r = e.target.getBoundingClientRect(), sc = r.height / T.h;
		var row = Math.floor(((e.clientY - r.top) / sc - T.pad) / T.ch);
		if (row >= 0 && row < T.rows) events.push(0x10000 + row);
	}
	function putSave(file, data) { Module.FS.writeFile(SAVE, data); }
	function clearSave() { try { Module.FS.unlink(SAVE); } catch (e) { } }

	/* ---------- fonts ---------- */
	function loadFace(n, now) {
		var redraw = function () {
			for (var p = 1; p < panes.length; p++) if (panes[p]) shape(p);
			document.querySelector('#t-vis .body').style.fontFamily = L.face ? '"' + L.face + '", monospace' : '';
			applyDom();
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
		fetch('fonts.json').then(function (r) { return r.json(); }).then(function (list) {
			var sel = $('sel-font');
			list.forEach(function (n) { var o = document.createElement('option'); o.value = n; o.textContent = n.replace(/^Web(Plus|437)_/, '').replace(/_/g, ' '); sel.appendChild(o); });
			sel.value = (L && L.face) || '';
		}).catch(function () { });
		$('sel-font').onchange = function () { if (!L) return; L.face = this.value; saveLayout(); loadFace(this.value, true); this.blur(); };
		$('btn-restart').onclick = function () { location.reload(); };
		document.querySelector('#pop canvas').addEventListener('mousedown', onPopClick);
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
				if (d.tile !== L.tile) { L.tile = d.tile; shape(P_MAP); }
			}
			applyDom();
		}, 150);
	});
})();
