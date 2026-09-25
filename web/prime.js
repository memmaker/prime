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
	var SAVE = DIR + '/save/player.sav', LAYOUT_FILE = DIR + '/web-layout.json';
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
	var FONT_MIN = 8, FONT_MAX = 28;
	/* the key codes of port/XUI.cpp (the keymaps name them "up arrow", ...) */
	var KEY = { ArrowUp: 0x101, ArrowDown: 0x102, ArrowLeft: 0x103, ArrowRight: 0x104,
		Home: 0x105, End: 0x106, PageUp: 0x107, PageDown: 0x108, Insert: 0x10A, Delete: 0x10B };

	var panes = [];            /* {cv, ctx, cols, rows, cw, ch, pad, buf} */
	var events = [];
	var mapImg = document.createElement('canvas'), mapCtx;   /* the whole map at 32 px */
	mapImg.width = MAP_COLS * TS; mapImg.height = MAP_ROWS * TS;
	mapCtx = mapImg.getContext('2d');
	var running = false, saveReq = false;
	var cur = { y: -1, x: -1 };
	var dpr = Math.max(1, Math.min(3, window.devicePixelRatio || 1));
	var L = null, rects = {};

	function $(id) { return document.getElementById(id); }
	function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
	function status(msg, isError) {
		var s = $('status');
		s.textContent = msg;
		s.className = isError ? 'error' : '';
		s.hidden = !msg;
	}

	/* ---------- panes ---------- */

	function measure(px) {
		var c = document.createElement('canvas').getContext('2d');
		c.font = px + 'px ' + FONT;
		return Math.ceil(c.measureText('M').width);
	}

	/* Cell size from the zoom settings; rebuilds the canvas and redraws */
	function shape(p) {
		var T = panes[p];
		if (p === P_MAP) { T.cw = L.tile; T.ch = L.tile; T.pad = 0; }
		else {
			var f = p === P_POP ? L.font.pop : L.font[WIN[p]];
			T.cw = measure(f); T.ch = Math.round(f * 1.3); T.pad = p === P_POP ? T.cw : 0;
			T.font = f + 'px ' + FONT;
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
		var font = W >= 1600 ? 14 : 13, tile = TILE_STEPS[0];
		var sideW = SIDE_COLS * measure(font) + BORDER + 4;
		/* the map may scroll sideways: size the tiles by its height */
		TILE_STEPS.forEach(function (t) { if (MAP_ROWS * t + BORDER <= H * 0.72 && t <= 48) tile = t; });
		var mapH = MAP_ROWS * tile + BORDER;
		return { v: 1, tile: tile, auto: true, font: { msg: font, stat: font, inv: font, pop: font },
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
				Object.keys(d.font).forEach(function (k) {
					if (s.font && s.font[k] >= FONT_MIN && s.font[k] <= FONT_MAX) d.font[k] = s.font[k];
				});
			}
		} catch (err) { /* nothing saved yet */ }
		L = d;
	}

	var saveTimer = 0;
	function saveLayout() {
		clearTimeout(saveTimer);
		saveTimer = setTimeout(function () {
			try { Module.FS.writeFile(LAYOUT_FILE, JSON.stringify(L)); syncFiles(); }
			catch (err) { console.warn('layout not saved', err); }
		}, 400);
	}

	function computeRects() {
		var A = areaSize(), W = A.w, H = A.h, h = GUT / 2, s = L.split;
		var xs = clamp(Math.round(W * s.side), 160, W - 90);
		var yb = clamp(Math.round(H * s.bottom), 120, H - 64);
		var yt = clamp(Math.round(H * s.stat), 64, H - 64);
		return {
			map: [0, 0, xs - h, yb - h],
			msg: [0, yb + h, xs - h, H - yb - h],
			stat: [xs + h, 0, W - xs - h, yt - h],
			inv: [xs + h, yt + h, W - xs - h, H - yt - h],
			split: { side: [xs - h, 0, GUT, H], bottom: [0, yb - h, xs - h, GUT], stat: [xs + h, yt - h, W - xs - h, GUT] }
		};
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
			var m = rects.map, A = areaSize();
			box = { w: A.w - m[0] - L.tile, h: A.h - 8 };
		} else {
			var r = rects[WIN[p]];
			if (!r) return;
			box = { w: r[2] - BORDER, h: r[3] - BORDER - (p === P_MAP ? 0 : TITLE_H) };
		}
		/* the map never shrinks: bigger than its window, it scrolls with the hero */
		if (p === P_MAP) { T.box = box; scrollMap(true); return; }
		var sc = Math.min(1, box.w / T.w, box.h / T.h);
		T.cv.style.width = T.w * sc + 'px';
		T.cv.style.height = T.h * sc + 'px';
		if (p === P_POP) { var pop = $('pop'); pop.style.left = rects.map[0] + L.tile / 2 + 'px'; pop.style.top = rects.map[1] + 4 + 'px'; }
	}

	var hero = { y: -1, x: -1 }, off = { x: 0, y: 0 };
	/* Keep the hero in the middle half of the map window; recentre when it
	 * leaves it (or always, after a zoom, resize or new level) */
	function scrollMap(force) {
		var T = panes[P_MAP];
		if (!T || !T.box) return;
		T.cv.style.width = T.w + 'px'; T.cv.style.height = T.h + 'px';
		['x', 'y'].forEach(function (a) {
			var size = a === 'x' ? T.w : T.h, view = a === 'x' ? T.box.w : T.box.h;
			var c = (a === 'x' ? hero.x * T.cw + T.cw / 2 : hero.y * T.ch + T.ch / 2) - off[a];
			if (hero.x < 0) return;
			if (size <= view) off[a] = 0;
			else if (force || c < view / 4 || c > view * 3 / 4)
				off[a] = clamp(Math.round(c + off[a] - view / 2), 0, size - view);
		});
		T.cv.style.marginLeft = -off.x + 'px';
		T.cv.style.marginTop = -off.y + 'px';
	}

	function applyDom() {
		rects = computeRects();
		WIN.forEach(function (id, p) { place($('t-' + id), rects[id]); fit(p); });
		SPLITS.forEach(function (k) { place($('split-' + k), rects.split[k]); });
		fit(P_POP);
	}

	function startDrag(k, e) {
		var el = $('split-' + k);
		el.setPointerCapture(e.pointerId);
		el.classList.add('drag');
		function move(ev) {
			var g = $('game').getBoundingClientRect(), H = g.height;
			if (k === 'bottom') L.split.bottom = clamp((ev.clientY - g.top) / H, 0.1, 0.95);
			if (k === 'stat') L.split.stat = clamp((ev.clientY - g.top) / H, 0.05, 0.95);
			if (k === 'side') L.split.side = clamp((ev.clientX - g.left) / g.width, 0.3, 0.97);
			L.auto = false;
			applyDom();
		}
		function up() {
			el.classList.remove('drag');
			el.removeEventListener('pointermove', move);
			el.removeEventListener('pointerup', up);
			saveLayout();
		}
		el.addEventListener('pointermove', move);
		el.addEventListener('pointerup', up);
		e.preventDefault();
	}

	function zoomMap(d) {
		var i = clamp(TILE_STEPS.indexOf(L.tile) + d, 0, TILE_STEPS.length - 1);
		L.tile = TILE_STEPS[i]; L.auto = false;
		shape(P_MAP); applyDom(); saveLayout();
		status('Map tiles: ' + L.tile + ' px');
		setTimeout(function () { status(''); }, 1200);
	}

	function zoomText(id, d) {
		var ids = [id];
		ids.forEach(function (k) { L.font[k] = clamp(L.font[k] + d, FONT_MIN, FONT_MAX); });
		L.font.pop = L.font[ids[0]];            /* pop-ups follow the last zoomed window */
		WIN.forEach(function (w, p) { if (p && ids.indexOf(w) >= 0) shape(p); });
		if (panes[P_POP]) shape(P_POP);
		applyDom(); saveLayout();
	}

	function resetLayout() {
		L = defaultLayout();
		for (var p = 0; p < panes.length; p++) if (panes[p]) shape(p);
		applyDom(); saveLayout();
	}

	/* ---------- called by the game (port/XUI.cpp) ---------- */

	var pr = {
		init: function (p, cols, rows) {
			if (!L) loadLayout();
			makePane(p, cols, rows);
			if (p === P_INV) { $('game').hidden = false; applyDom(); }
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
		key: function () { return events.length ? events.shift() : -1; },
		pending: function () { return events.length ? 1 : 0; },
		requestSave: function () { saveReq = true; },   /* also for testing */
		wantSave: function () {
			if (!saveReq || !running) return 0;
			saveReq = false;
			return 1;
		},
		saved: function () { syncFiles(); },
		end: function (saved) {
			running = false;
			syncFiles(function () {
				$('overlay-msg').textContent = saved ? 'Your game has been saved. Play again to continue it.'
					: 'The game is over. Play again for a new character.';
				$('overlay').hidden = false;
			});
		}
	};

	/* ---------- input ---------- */
	function onKey(e) {
		if (!$('help').hidden) {
			if (e.key === 'Escape') { $('help').hidden = true; e.preventDefault(); }
			return;
		}
		if (!running || e.isComposing || e.metaKey) return;
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
	var syncing = false, syncAgain = false, pendingCbs = [];
	function syncFiles(cb) {
		if (!Module.FS) { if (cb) cb(); return; }
		if (typeof cb === 'function') pendingCbs.push(cb);
		if (syncing) { syncAgain = true; return; }
		syncing = true;
		var cbs = pendingCbs; pendingCbs = [];
		Module.FS.syncfs(false, function (err) {
			syncing = false;
			if (err) status('Saving to browser storage (IndexedDB) failed: ' + err + '. Use "Export save" to keep a copy.', true);
			cbs.forEach(function (f) { f(err); });
			if (syncAgain) { syncAgain = false; syncFiles(); }
		});
	}
	function hasSave() { try { Module.FS.stat(SAVE); return true; } catch (e) { return false; } }
	/* a click on a pop-up row: the game maps the row to a menu entry */
	function onPopClick(e) {
		var T = panes[P_POP];
		if (!T || !running) return;
		var r = e.target.getBoundingClientRect(), sc = r.height / T.h;
		var row = Math.floor(((e.clientY - r.top) / sc - T.pad) / T.ch);
		if (row >= 0 && row < T.rows) events.push(0x10000 + row);
	}
	function exportSave() {
		if (running) saveReq = true;
		setTimeout(function () {
			if (!hasSave()) { status('There is no saved game yet.', true); return; }
			var a = document.createElement('a');
			a.href = URL.createObjectURL(new Blob([Module.FS.readFile(SAVE)], { type: 'application/octet-stream' }));
			a.download = 'prime.sav';
			document.body.appendChild(a); a.click();
			setTimeout(function () { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
		}, running ? 1500 : 0);
	}
	function importSave(file) {
		var r = new FileReader();
		r.onload = function () {
			if (!confirm('Replace the current game with "' + file.name + '"?')) return;
			running = false;
			Module.FS.writeFile(SAVE, new Uint8Array(r.result));
			syncFiles(function (err) { if (!err) location.reload(); });
		};
		r.readAsArrayBuffer(file);
	}
	function newGame() {
		if (!confirm('Delete the saved game in this browser and start a new one?')) return;
		running = false;
		[SAVE].forEach(function (f) { try { Module.FS.unlink(f); } catch (e) { } });
		syncFiles(function (err) { if (!err) location.reload(); });
	}

	/* ---------- help ---------- */
	var helpLoaded = false;
	function toggleHelp() {
		var h = $('help');
		h.hidden = !h.hidden;
		if (!h.hidden && !helpLoaded) {
			helpLoaded = true;
			fetch('help.html').then(function (r) { if (!r.ok) throw new Error(r.status); return r.text(); })
				.then(function (t) { $('help-body').innerHTML = t; })
				.catch(function (err) { helpLoaded = false; $('help-body').textContent = 'Could not load the guide (' + err + '). Press ? in the game for its own help.'; });
		}
		if (!h.hidden) $('help-body').focus();
	}

	/* ---------- startup ---------- */
	window.Module = {
		pr: pr,
		preRun: [function () {
			var FS = Module.FS;
			FS.mkdirTree(DIR);
			FS.mount(Module.IDBFS, {}, DIR);
			FS.chdir(ROOT);                      /* the game's relative data/, user/, score/ */
			Module.addRunDependency('idbfs');
			FS.syncfs(true, function (err) {
				if (err) status('Could not read saved games from IndexedDB (' + err + '). Saving may not work in this browser mode.', true);
				['save', 'score'].forEach(function (d) { try { FS.mkdir(DIR + '/' + d); } catch (e) { } });
				try { FS.symlink(DIR + '/score', ROOT + '/score'); } catch (e) { }
				/* the keymaps come with the game, not from storage */
				try { FS.unlink(DIR + '/keymap'); } catch (e) { }
				FS.symlink(ROOT + '/keymap', DIR + '/keymap');
				Module.removeRunDependency('idbfs');
			});
		}],
		arguments: ['-u', 'player'],
		onRuntimeInitialized: function () {
			running = true;
			saveReq = true;                      /* PRIME deleted the save it loaded: write it back */
			status('');
		},
		print: function (s) { console.log(s); },
		printErr: function (s) { console.warn(s); },
		setStatus: function (s) { if (s && !running) status(s.replace(/\(\d+\/\d+\)/, '').trim() || 'Loading…'); },
		onAbort: function (what) { crashed(what); }
	};

	function crashed(err) {
		if (!running) return;
		running = false;
		var msg = (err && (err.message || err.reason && err.reason.message)) || String(err);
		console.error('[prime] crash: ' + msg + '\n' + (err && (err.stack || err.reason && err.reason.stack) || ''));
		status('The game crashed (' + msg + '). Reload the page to continue from the last autosave.', true);
	}
	window.addEventListener('unhandledrejection', function (e) {
		/* exit() unwinds with an ExitStatus; that is the normal end */
		if (e.reason && e.reason.name === 'ExitStatus') return;
		crashed(e.reason);
	});
	window.addEventListener('error', function (e) {
		if (e.error && e.error.name === 'ExitStatus') return;
		if (e.error instanceof WebAssembly.RuntimeError || /prime-core/.test(e.filename || '')) crashed(e.error || e.message);
	});

	/* autosave: every 2 minutes and when the page is hidden */
	setInterval(function () { saveReq = true; }, 120000);
	document.addEventListener('visibilitychange', function () { if (document.hidden) saveReq = true; });
	window.addEventListener('beforeunload', function (e) { if (running) { e.preventDefault(); e.returnValue = ''; } });

	document.addEventListener('keydown', onKey);
	document.addEventListener('DOMContentLoaded', function () {
		$('btn-export').onclick = exportSave;
		$('btn-import').onclick = function () { $('import-file').click(); };
		$('import-file').onchange = function () { if (this.files[0]) importSave(this.files[0]); this.value = ''; };
		$('btn-new').onclick = newGame;
		$('btn-help').onclick = toggleHelp;
		$('help-close').onclick = toggleHelp;
		$('btn-zoom-in').onclick = function () { zoomMap(1); };
		$('btn-zoom-out').onclick = function () { zoomMap(-1); };
		$('btn-layout').onclick = resetLayout;
		$('btn-restart').onclick = function () { location.reload(); };
		document.querySelector('#pop canvas').addEventListener('mousedown', onPopClick);
		document.querySelectorAll('button').forEach(function (b) {
			b.addEventListener('mousedown', function (e) { e.preventDefault(); });
		});
		SPLITS.forEach(function (k) { $('split-' + k).addEventListener('pointerdown', function (e) { startDrag(k, e); }); });
		['msg', 'stat', 'inv'].forEach(function (id) {
			var w = $('t-' + id);
			w.querySelector('.zin').addEventListener('click', function () { zoomText(id, 1); });
			w.querySelector('.zout').addEventListener('click', function () { zoomText(id, -1); });
		});
	});
	var resizeTimer = 0;
	window.addEventListener('resize', function () {
		if (!L) return;
		clearTimeout(resizeTimer);
		resizeTimer = setTimeout(function () {
			if (L.auto) {                        /* not customised: follow the window */
				var d = defaultLayout();
				L.split = d.split;
				if (d.tile !== L.tile) { L.tile = d.tile; shape(P_MAP); }
			}
			applyDom();
		}, 150);
	});
})();
