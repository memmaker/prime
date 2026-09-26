/* X11 frontend for PRIME (RVIP): one window per pane, like the other ports
 * in ~/Games. Replaces NotEye: the map is drawn from the same per-cell tile
 * stacks NEUI.cpp builds (gfx/primetiles.png, 32x32, nearest-neighbour),
 * everything else is text in its own window:
 *   Map (tiles, scrolls with the hero)  Status (kSide)
 *   Messages (history + kLog)           Inventory (from the pack)
 *   Pop-up (kTemp / kMenu / kMenuHelp, sized to its content, over the map)
 * Env: PRIME_TILES (port/tiles.rgba), PRIME_VIEW (map columns shown, 38),
 * PRIME_SCALE (tile scale, 1), PRIME_XFT (font, Menlo), PRIME_TEXT (px, 15),
 * PRIME_MAP / _STATUS / _MSG / _INV = "x,y" window positions,
 * PRIME_DUMP=<file> (text of every pane on each refresh, for testing). */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>   /* browser: the page (web/prime.js) draws the panes */
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#endif
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

#include "Interface.h"
#include "Hero.h"
#include "Effect.h"
#include "Object.h"
#include "XUI.h"
#include "Game.h"
#include <unistd.h>

/* Special keys (names in KeyNames below, used by the keymap files). */
enum {
    XK_UP = 0x101, XK_DOWN, XK_LEFT, XK_RIGHT, XK_HOME, XK_END, XK_PGUP,
    XK_PGDN, XK_CENTER, XK_INS, XK_DEL, XK_F0 = 0x110, XK_MAXKEY = 0x130
};

/* NotEye spatial flags: unused here (flat view), kept so the tile code
   below stays as in NEUI.cpp. */
enum { spFlat = 0, spFloor = 0, spIFloor = 0, spCeil = 0, spWallN = 0,
       spWallE = 0, spWallS = 0, spWallW = 0, spIWallL = 0, spIWallR = 0,
       spICeil = 0, spMonst = 0, spIItem = 0, spItem = 0, spFree = 0,
       spCenter = 0 };

struct shCache {
    bool needs_update;
    std::vector<int> t; /* x, y, spatial, recolor per layer */
    void add (int x, int y, int spatial, int recolor = 0)
    { t.push_back (x); t.push_back (y); t.push_back (spatial); t.push_back (recolor); }
    void dellast () { if (t.size () >= 4) t.resize (t.size () - 4); }
    void reset () { t.clear (); needs_update = true; }
};

struct Cell { unsigned char ch, fg, bg; };
struct TWin {
    int x1, y1, x2, y2, cx, cy;
    int open; /* 0 = closed, else opening order */
    Cell c[25][80];
};

struct shXInterface : public shInterface
{
    shXInterface ();
    ~shXInterface ();

    int getChar ();
    int getSpecialChar (SpecialKey *sk);
    int getStr (char *buf, int len, const char *prompt, const char *dflt = NULL);
    const char *getKeyForCommand (Command cmd);
    shVector<const char *> *getKeysForCommand (Command cmd);

    void cursorOnXY (int x, int y, int curstype);
    void draw (int x, int y, shCreature *c,
               shObjectIlk *oi, shObject *o, shFeature *f, shSquare *s);
    void drawMem (int x, int y, shCreature *c,
                  shObjectIlk *o, shFeature *f, shSquare *s);
    void drawMem (int x, int y) { drawMem (x, y, NULL, NULL, NULL, NULL); }
    void drawEffect (int x, int y, eff::type e);

    int diag (const char *format, ...);
    void drawScreen ();
    void refreshScreen ();
    void drawLog ();
    void pageLog ();
    void showVersion ();
    void hideVersion ();
    void doScreenShot (FILE *file);

    int getMaxLines () { return 25; }
    int getMaxColumns () { return 80; }
    void newWin (Window win);
    void delWin (Window win);
    void moveWin (Window win, int x1, int y1, int x2, int y2);
    void clearWin (Window win);
    void refreshWin (Window) { present (); }
    void setWinColor (Window win, shColor fg, shColor bg);
    void winGoToYX (Window win, int y, int x);
    void winGetYX (Window win, int *y, int *x);
    void winPutchar (Window win, const char c);
    void winPrint (Window win, const char *fmt, ...);
    void winPrint (Window win, int y, int x, const char *fmt, ...);

    void runMainLoop () { mainLoop (); }

    shCache mCache[MAPMAXCOLUMNS][MAPMAXROWS];
    TWin mW[kMaxWin];
    int mOpenSeq;
    unsigned char mFg, mBg;
    int mCurX, mCurY, mCursType;

    void present ();

 private:
    Command mKey2Cmd[XK_MAXKEY];

    Command keyToCommand (int key) { return key >= 0 && key < XK_MAXKEY ? mKey2Cmd[key] : kNoCommand; }
    void readKeybindings (const char *fname);
    void assign (int key, Command cmd) { if (key >= 0 && key < XK_MAXKEY) mKey2Cmd[key] = cmd; }
    void resetKeybindings ()
    { for (int i = 0; i < XK_MAXKEY; ++i) mKey2Cmd[i] = kNoCommand; }

    bool has_vi_keys () { return mKey2Cmd[int ('l')] == kMoveE; }
    bool has_vi_fire_keys () { return mKey2Cmd[0x1f & 'l'] == kFireE; }

    void terr2tile (shTerrainType terr, shCache *cache, int odd, int recolor);
    void feat2tile (shFeature *feat, shCache *cache, int recolor);
    void mapGlyph (int x, int y, shGlyph g);
    void put (Window win, char ch);
};

static shXInterface *UI;

/* The hero stands on a level (the game asks it about shops etc.). */
static bool
heroPlaced ()
{
    shCreature *h = Hero.cr ();
    return h && Level && h->mX >= 0 && h->mX < MAPMAXCOLUMNS && h->mY >= 0 && h->mY < MAPMAXROWS;
}

shInterface *
startX11 ()
{
    return UI = new shXInterface ();
}

/**********************************************************************
 * X11 side
 */

#define TS 32 /* tile size in the sheet */
enum { P_MAP, P_STATUS, P_MSG, P_INV, P_POP, NPANES };
static const char *pname[NPANES] = { "MAP", "STATUS", "MSG", "INV", "POP" };
static const char *ptitle[NPANES] = { "PRIME", "PRIME Status", "PRIME Messages", "PRIME Inventory", "" };

#ifndef __EMSCRIPTEN__
static Display *dpy;
static int scr;
static Visual *vis;
static Colormap cmap;
static GC gc;
static XftFont *txtfont, *boldfont;
static XftColor xcol[16];
static unsigned long pix[16];
static XImage *img; /* one map cell, reused */
#endif
static int scale = 1, cw, tw, th, viewCols = 40, viewX0 = -1;
static unsigned char *sheet; /* RGBA */
static int sheet_w, sheet_h;

static struct pane {
#ifndef __EMSCRIPTEN__
    ::Window win;
    Pixmap pix;
    XftDraw *xd;
#endif
    int cols, rows, cw, ch, pad;
    std::vector<Cell> shown; /* what the pixmap shows, to skip redraws */
} P[NPANES];

/* VGA palette in shColor order. */
static const unsigned char pal[16][3] = {
    { 0, 0, 0 }, { 0, 0, 170 }, { 0, 170, 0 }, { 0, 170, 170 },
    { 170, 0, 0 }, { 170, 0, 170 }, { 170, 85, 0 }, { 170, 170, 170 },
    { 85, 85, 85 }, { 85, 85, 255 }, { 85, 255, 85 }, { 85, 255, 255 },
    { 255, 85, 85 }, { 255, 85, 255 }, { 255, 255, 85 }, { 255, 255, 255 },
};
/* Text colours: dark blue is unreadable on black, lift it a bit. */
static const unsigned char tpal[16][3] = {
    { 0, 0, 0 }, { 70, 90, 230 }, { 0, 175, 0 }, { 0, 175, 175 },
    { 190, 30, 30 }, { 180, 50, 180 }, { 180, 110, 20 }, { 185, 185, 185 },
    { 110, 110, 110 }, { 110, 130, 255 }, { 90, 255, 90 }, { 90, 255, 255 },
    { 255, 95, 95 }, { 255, 110, 255 }, { 255, 255, 90 }, { 255, 255, 255 },
};

static void
load_sheet ()
{
    const char *p = getenv ("PRIME_TILES");
    FILE *f = fopen (p ? p : "port/tiles.rgba", "rb");
    unsigned char h[8];
    if (!f || fread (h, 1, 8, f) != 8) { if (f) fclose (f); return; }
    sheet_w = h[0] | h[1] << 8 | h[2] << 16 | h[3] << 24;
    sheet_h = h[4] | h[5] << 8 | h[6] << 16 | h[7] << 24;
    sheet = (unsigned char *) malloc ((size_t) sheet_w * sheet_h * 4);
    if (fread (sheet, 4, (size_t) sheet_w * sheet_h, f) != (size_t) sheet_w * sheet_h) {
        free (sheet);
        sheet = NULL;
    }
    fclose (f);
}

#ifdef __EMSCRIPTEN__
EM_JS(void, js_init, (int p, int c, int r), { Module.pr.init(p, c, r); });
EM_JS(void, js_put, (int p, int y, int x, int ch), { Module.pr.put(p, y, x, ch); });
EM_JS(void, js_tile, (int x, int y, const unsigned char *rgba), { Module.pr.tile(x, y, rgba); });
EM_JS(void, js_popup, (int r, int c), { Module.pr.popup(r, c); });
EM_JS(void, js_flush, (int fy, int fx, int cy, int cx), { Module.pr.flush(fy, fx, cy, cx); });
EM_JS(int, js_key, (int at_cmd), { return Module.pr.key(at_cmd); });
EM_JS(void, js_prompt, (const char *s), { Module.pr.prompt(UTF8ToString(s)); });
EM_JS(int, js_want_save, (void), { return Module.pr.wantSave(); });
EM_JS(void, js_end, (int saved), { Module.pr.end(saved); });

/* Run report (roguelikes-index/server/CONTRACT.md): fire-and-forget GET,
   never throws, offline just fails silently. Negative ints are omitted. */
EM_JS(void, js_beacon, (const char *g, const char *ev, const char *name, const char *killer, int depth, int score, int turns, int lvl), {
    try {
        var p = [['g', UTF8ToString(g)], ['ev', UTF8ToString(ev)], ['name', name ? UTF8ToString(name) : ''],
                 ['killer', killer ? UTF8ToString(killer) : ''], ['depth', depth], ['score', score], ['turns', turns], ['lvl', lvl]];
        var q = p.filter(function (a) { return a[1] !== '' && !(a[1] < 0); })
                 .map(function (a) { return a[0] + '=' + encodeURIComponent(a[1]); }).join('&');
        if (window.RvipWM && RvipWM.report) RvipWM.report(q); else fetch('/roguelikes/beacon?' + q, { keepalive: true, mode: 'no-cors' }).catch(function () {});
    } catch (e) {}
});
/* shHero::death, after logGame (mScore is what the high score table keeps) */
void
be_run_end (shCauseOfDeath how, shCreature *killer, const char *k)
{
    const char *ev = how == kWonGame ? "win" : how == kQuitGame ? "quit" : "death";
    if (how == kWonGame or how == kQuitGame) k = NULL;
    else if (killer and !killer->isHero ()) k = killer->myIlk ()->mName;
    if (k and !strncmp (k, "a ", 2)) k += 2;
    else if (k and !strncmp (k, "an ", 3)) k += 3;
    else if (k and !strncasecmp (k, "the ", 4)) k += 4;
    shCreature *h = Hero.cr ();
    js_beacon ("prime", ev, h->mName, k, Level ? Level->mDLevel : -1,
               Hero.tallyScore (), Clock / FULLTURN, h->mCLevel);
}

/* Visible window (RVIP 5b): creatures the hero sees and objects on seen
 * squares, in the game's own colours */
EM_JS(void, js_vis, (const char *s), { if (Module.pr.vis) Module.pr.vis(UTF8ToString(s)); });
static const char *vcolor (int c)
{
    static const char *pal[] = { "#000", "#35d", "#3b3", "#3cc", "#c33", "#c3c", "#a60", "#bbb", "#777", "#58f", "#5f5", "#5ff", "#f84", "#f5f", "#ff5", "#fff" };
    return c >= 0 && c < (int) (sizeof pal / sizeof *pal) ? pal[c] : "";
}

static void sendVisible ()
{
    static char buf[4096];
    int n = 0;
    if (!Level || !Hero.cr ()) { js_vis (""); return; }
    for (int i = 0; i < Level->mCrList.count () && n < 3900; i++) {
        shCreature *c = Level->mCrList.get (i);
        if (!c || c == Hero.cr () || !Hero.cr ()->canSee (c)) continue;
        n += snprintf (buf + n, sizeof buf - n, "M%c%s\t%s\n", c->mGlyph.mSym, c->an (), vcolor (c->mGlyph.mColor));
    }
    for (int x = 0; x < MAPMAXCOLUMNS; x++)
        for (int y = 0; y < MAPMAXROWS && n < 3900; y++) {
            shObjectVector *v = Level->mObjects[x][y];
            if (!v || !Hero.cr ()->canSee (x, y)) continue;
            for (int i = 0; i < v->count () && n < 3900; i++) {
                shObject *o = v->get (i);
                shGlyph g = o->getGlyph ();
                n += snprintf (buf + n, sizeof buf - n, "I%c%s\t%s\n", g.mSym, o->getDescription (), vcolor (g.mColor));
            }
        }
    buf[n] = 0;
    js_vis (buf);
}
static bool dpy = true;   /* "display open" for the shared code */

static void
open_display ()
{
    viewCols = MAPMAXCOLUMNS;  /* the page scrolls the whole map */
    cw = TS;
    load_sheet ();
}

static void
pane_init (int p, int cols, int rows)
{
    struct pane *q = &P[p];
    q->cols = cols;
    q->rows = rows;
    q->shown.assign (cols * rows, Cell ());
    for (size_t i = 0; i < q->shown.size (); ++i) { q->shown[i].ch = ' '; q->shown[i].fg = 7; }
    js_init (p, cols, rows);
}

static void
popup (int rows, int cols)
{
    struct pane *q = &P[P_POP];
    if (!rows) {
        if (q->cols) js_popup (0, 0);
        q->cols = q->rows = 0;
        return;
    }
    if (q->cols == cols && q->rows == rows) return;
    js_popup (rows, cols);    /* a blank pane of that size */
    q->cols = cols;
    q->rows = rows;
    q->shown.assign (cols * rows, Cell ());
    for (size_t i = 0; i < q->shown.size (); ++i) { q->shown[i].ch = ' '; q->shown[i].fg = 7; }
}
#else
static XftFont *
font (double px, int weight)
{
    const char *fam = getenv ("PRIME_XFT");
    return XftFontOpen (dpy, scr, XFT_FAMILY, XftTypeString, fam ? fam : "Menlo",
                        XFT_WEIGHT, XftTypeInteger, weight,
                        XFT_PIXEL_SIZE, XftTypeDouble, px, NULL);
}

static void
open_display ()
{
    XGlyphInfo gi;
    const char *e;
    if (!(dpy = XOpenDisplay (NULL))) { fprintf (stderr, "prime: no X display\n"); exit (1); }
    scr = DefaultScreen (dpy);
    vis = DefaultVisual (dpy, scr);
    cmap = DefaultColormap (dpy, scr);
    if ((e = getenv ("PRIME_SCALE")) && atoi (e) > 0) scale = atoi (e);
    if ((e = getenv ("PRIME_VIEW")) && atoi (e) > 0) viewCols = atoi (e);
    if (viewCols > MAPMAXCOLUMNS) viewCols = MAPMAXCOLUMNS;
    cw = TS * scale;
    for (int i = 0; i < 16; i++) {
        XRenderColor c = { (unsigned short) (tpal[i][0] * 257), (unsigned short) (tpal[i][1] * 257),
                           (unsigned short) (tpal[i][2] * 257), 0xffff };
        XColor xc;
        XftColorAllocValue (dpy, vis, cmap, &c, &xcol[i]);
        xc.red = c.red; xc.green = c.green; xc.blue = c.blue;
        XAllocColor (dpy, cmap, &xc);
        pix[i] = xc.pixel;
    }
    XGCValues gv;
    gv.graphics_exposures = False; /* no NoExpose events from XCopyArea */
    gc = XCreateGC (dpy, DefaultRootWindow (dpy), GCGraphicsExposures, &gv);
    double px = (e = getenv ("PRIME_TEXT")) ? atof (e) : 15;
    txtfont = font (px, XFT_WEIGHT_MEDIUM);
    boldfont = font (px, XFT_WEIGHT_BOLD);
    XftTextExtents8 (dpy, txtfont, (FcChar8 *) "M", 1, &gi);
    tw = gi.xOff;
    th = txtfont->ascent + txtfont->descent;
    img = XCreateImage (dpy, vis, DefaultDepth (dpy, scr), ZPixmap, 0,
                        (char *) malloc (cw * cw * 4), cw, cw, 32, 0);
    load_sheet ();
}

/* Default layout for the 1440x932 screen (XQuartz adds ~28 px title
   bars): map top left, Status right of it, Messages and Inventory below. */
static void
place (int p, int *x, int *y)
{
    char var[32];
    const char *e;
    int below = P[P_MAP].rows * cw + 28;
    int msgw = P[P_MSG].cols * tw + 2 * P[P_MSG].pad;
    *x = p == P_STATUS ? P[P_MAP].cols * cw + 6 : p == P_INV ? msgw + 6 : 0;
    *y = p == P_MSG || p == P_INV ? below : 0;
    snprintf (var, sizeof var, "PRIME_%s", pname[p]);
    if ((e = getenv (var))) sscanf (e, "%d,%d", x, y);
}

static void
make_pixmap (struct pane *q)
{
    int w = q->cols * q->cw + 2 * q->pad, h = q->rows * q->ch + 2 * q->pad;
    if (q->xd) XftDrawDestroy (q->xd);
    if (q->pix) XFreePixmap (dpy, q->pix);
    q->pix = XCreatePixmap (dpy, q->win, w, h, DefaultDepth (dpy, scr));
    q->xd = XftDrawCreate (dpy, q->pix, vis, cmap);
    XSetForeground (dpy, gc, pix[0]);
    XFillRectangle (dpy, q->pix, gc, 0, 0, w, h);
    q->shown.assign (q->cols * q->rows, Cell ());
    for (size_t i = 0; i < q->shown.size (); ++i) q->shown[i].ch = ' ';
}

static void
pane_init (int p, int cols, int rows)
{
    struct pane *q = &P[p];
    XSizeHints h;
    int x, y;
    q->cols = cols;
    q->rows = rows;
    q->cw = p == P_MAP ? cw : tw;
    q->ch = p == P_MAP ? cw : th;
    q->pad = p == P_MAP ? 0 : tw / 2;
    place (p, &x, &y);
    int w = cols * q->cw + 2 * q->pad, hh = rows * q->ch + 2 * q->pad;
    q->win = XCreateSimpleWindow (dpy, DefaultRootWindow (dpy), x, y, w, hh, 0, pix[7], pix[0]);
    /* Fixed size: no resize fight with XQuartz. */
    h.flags = PPosition | USPosition | PMinSize | PMaxSize;
    h.x = x;
    h.y = y;
    h.min_width = h.max_width = w;
    h.min_height = h.max_height = hh;
    XSetWMNormalHints (dpy, q->win, &h);
    XStoreName (dpy, q->win, ptitle[p]);
    XSelectInput (dpy, q->win, KeyPressMask | ExposureMask | ButtonPressMask);
    make_pixmap (q);
    XMapWindow (dpy, q->win);
}

/* The pop-up: no title bar, over the top left of the map, sized to its
   content plus half a character of padding. */
static void
popup (int rows, int cols)
{
    struct pane *q = &P[P_POP];
    int x, y, w, h;
    ::Window child;
    if (!rows) {
        if (q->win && q->cols) XUnmapWindow (dpy, q->win);
        q->cols = q->rows = 0;
        return;
    }
    if (q->win && q->cols == cols && q->rows == rows) return;
    q->cw = tw;
    q->ch = th;
    q->pad = tw / 2 + 2;
    q->cols = cols;
    q->rows = rows;
    w = cols * tw + 2 * q->pad;
    h = rows * th + 2 * q->pad;
    XTranslateCoordinates (dpy, P[P_MAP].win, DefaultRootWindow (dpy), cw / 2, cw / 2, &x, &y, &child);
    if (!q->win) {
        XSetWindowAttributes a;
        a.override_redirect = True;
        a.background_pixel = pix[0];
        a.border_pixel = pix[7];
        q->win = XCreateWindow (dpy, DefaultRootWindow (dpy), x, y, w, h, 1, CopyFromParent, InputOutput,
                                CopyFromParent, CWOverrideRedirect | CWBackPixel | CWBorderPixel, &a);
        XSelectInput (dpy, q->win, KeyPressMask | ExposureMask | ButtonPressMask);
    } else
        XMoveResizeWindow (dpy, q->win, x, y, w, h);
    make_pixmap (q);
    XMapRaised (dpy, q->win);
}

#endif

static void
draw_text (int p, int y, int x, Cell c)
{
    struct pane *q = &P[p];
    if (y < 0 || x < 0 || y >= q->rows || x >= q->cols) return;
    Cell &s = q->shown[y * q->cols + x];
    if (s.ch == c.ch && s.fg == c.fg && s.bg == c.bg) return;
    s = c;
#ifdef __EMSCRIPTEN__
    js_put (p, y, x, (unsigned char) c.ch | (c.fg & 15) << 8 | (c.bg & 15) << 12);
#else
    int px = q->pad + x * q->cw, py = q->pad + y * q->ch;
    int fg = c.fg & 15, bg = c.bg & 15;
    if (bg) fg = 0; /* reverse video: black text on the colour */
    XSetForeground (dpy, gc, pix[bg]);
    XFillRectangle (dpy, q->pix, gc, px, py, q->cw, q->ch);
    if (c.ch == ' ' || !c.ch) return;
    FcChar8 ch = c.ch;
    XGlyphInfo gi;
    XftFont *f = fg == 15 || fg == 14 ? boldfont : txtfont;
    XftTextExtents8 (dpy, f, &ch, 1, &gi);
    XftDrawString8 (q->xd, &xcol[fg ? fg : (bg ? 0 : 7)], f, px + (q->cw - gi.xOff) / 2,
                    py + (q->ch - f->ascent - f->descent) / 2 + f->ascent, &ch, 1);
#endif
}

static void
text_line (int p, int y, const char *s, int fg)
{
    Cell c;
    c.fg = fg;
    c.bg = 0;
    for (int x = 0; x < P[p].cols; ++x) {
        c.ch = *s ? *s++ : ' ';
        draw_text (p, y, x, c);
    }
}

static void
blend_tile (unsigned char *buf, int tx, int ty, unsigned long col, int mult)
{
    if (!sheet || tx < 0 || ty < 0 || (tx + 1) * TS > sheet_w || (ty + 1) * TS > sheet_h) return;
    int cr = col >> 16 & 255, cg = col >> 8 & 255, cb = col & 255;
    for (int y = 0; y < TS; ++y)
        for (int x = 0; x < TS; ++x) {
            unsigned char *s = sheet + ((size_t) (ty * TS + y) * sheet_w + tx * TS + x) * 4;
            unsigned char *d = buf + (y * TS + x) * 3;
            int a = s[3];
            if (!a) continue;
            int r = s[0], g = s[1], b = s[2];
            if (col && mult) { /* NotEye recMult: white glyph -> colour */
                r = r * cr / 255; g = g * cg / 255; b = b * cb / 255;
            } else if (col) {  /* recDefault: tint */
                int l = (r * 3 + g * 6 + b) / 10;
                r = (l * cr / 255 + r) / 2; g = (l * cg / 255 + g) / 2; b = (l * cb / 255 + b) / 2;
            }
            d[0] = (r * a + d[0] * (255 - a)) / 255;
            d[1] = (g * a + d[1] * (255 - a)) / 255;
            d[2] = (b * a + d[2] * (255 - a)) / 255;
        }
}

static unsigned long
vga (int i)
{
    return (unsigned long) pal[i & 15][0] << 16 | pal[i & 15][1] << 8 | pal[i & 15][2];
}

/* Mirrors tileat () in src/lua/prime.lua. */
static void
layer (unsigned char *buf, int x, int y, int rc)
{
    extern shFlavor Flavors[];
    if ((y == 2 || (y >= kRowLittleA && y < kRowBigA + 26)) && x >= 24 && x <= 39) {
        /* Monster without a tile: its letter in its colour. */
        blend_tile (buf, 0, y, vga (x - 24) | (x == 24 ? 0x010101 : 0), 1);
    } else if (y == kRowGrenade && x >= 20) {
        shFlavor *fl = &Flavors[kFFirstGrenade + x - 20];
        blend_tile (buf, fl->mVague.mGlyph.mTileX, y, vga (fl->mAppearance.mGlyph.mColor), 1);
    } else if (y == kRow4DirAtt0 && x >= 24 && x <= 32) {
        blend_tile (buf, x - 23, y, 0xFF0000, 0);
    } else {
        blend_tile (buf, x, y, rc & 0xFFFFFF, 0);
    }
}

/* One map cell: the layers of its tile stack, each alpha-blended over
   the last, scaled nearest-neighbour. */
static void
draw_cell (int vx, int mx, int my)
{
    static unsigned char buf[TS * TS * 3];
    memset (buf, 0, sizeof buf);
    std::vector<int> &t = UI->mCache[mx][my].t;
    for (size_t i = 0; i + 3 < t.size (); i += 4)
        layer (buf, t[i], t[i + 1], t[i + 3]);
#ifdef __EMSCRIPTEN__
    static unsigned char rgba[TS * TS * 4];
    for (int i = 0; i < TS * TS; ++i) {
        rgba[i * 4] = buf[i * 3]; rgba[i * 4 + 1] = buf[i * 3 + 1];
        rgba[i * 4 + 2] = buf[i * 3 + 2]; rgba[i * 4 + 3] = 255;
    }
    js_tile (mx, my, rgba);
    (void) vx;
#else
    for (int y = 0; y < cw; ++y)
        for (int x = 0; x < cw; ++x) {
            unsigned char *s = buf + ((y / scale) * TS + x / scale) * 3;
            XPutPixel (img, x, y, (unsigned long) s[0] << 16 | s[1] << 8 | s[2]);
        }
    XPutImage (dpy, P[P_MAP].pix, gc, img, 0, 0, vx * cw, my * cw, cw, cw);
#endif
}

static void
draw_map ()
{
    /* Scroll so the cursor (hero, or the targeting cursor) stays at least
       a quarter of the view from either edge. */
    int fx = UI->mCurX < MAPMAXCOLUMNS ? UI->mCurX : (Hero.cr () ? Hero.cr ()->mX : 0);
    int margin = viewCols / 4, x0 = viewX0;
    if (x0 < 0 || fx < x0 + margin || fx >= x0 + viewCols - margin)
        x0 = fx - viewCols / 2;
    if (x0 > MAPMAXCOLUMNS - viewCols) x0 = MAPMAXCOLUMNS - viewCols;
    if (x0 < 0) x0 = 0;
    bool all = x0 != viewX0;
    viewX0 = x0;
    for (int y = 0; y < MAPMAXROWS; ++y)
        for (int x = 0; x < viewCols; ++x) {
            shCache &c = UI->mCache[x0 + x][y];
            if (all || c.needs_update) {
                draw_cell (x, x0 + x, y);
                c.needs_update = false;
            }
        }
}

/* Pane contents from the text windows. */
static void
draw_window_region (int p, TWin *w, int y0, int x0)
{
    for (int y = 0; y < P[p].rows; ++y)
        for (int x = 0; x < P[p].cols; ++x) {
            Cell c = { ' ', 7, 0 };
            if (y0 + y < 25 && x0 + x < 80) c = w->c[y0 + y][x0 + x];
            draw_text (p, y, x, c);
        }
}

static FILE *dumpf;

static void
dump_pane (const char *name, int p)
{
    if (!dumpf || !P[p].rows) return;
    fprintf (dumpf, "== %s\n", name);
    for (int y = 0; y < P[p].rows; ++y) {
        char line[256];
        int n = P[p].cols < 255 ? P[p].cols : 255;
        for (int x = 0; x < n; ++x) {
            unsigned char ch = P[p].shown[y * P[p].cols + x].ch;
            line[x] = ch ? ch : ' ';
        }
        while (n > 0 && line[n - 1] == ' ') --n;
        line[n] = 0;
        fprintf (dumpf, "%s\n", line);
    }
}

static void
draw_messages (shXInterface *ui, int histIdx, int wrapped, const char *hist)
{
    struct pane *q = &P[P_MSG];
    TWin *w = &ui->mW[shInterface::kLog];
    int live = 0;
    for (int y = 0; y < w->y2 - w->y1; ++y)
        for (int x = 0; x < 80; ++x)
            if (w->c[w->y1 + y][x].ch != ' ') live = y + 1;
    if (w->cy - w->y1 + 1 > live && (w->cx > 0)) live = w->cy - w->y1 + 1;
    int old = q->rows - live;
    /* History entries before the live rows (they are in it already). */
    int n = wrapped ? HISTORY_ROWS : histIdx;
    int skip = live;
    for (int y = 0; y < old; ++y) {
        int k = old - y + skip; /* entries back from the newest */
        if (k > n) { text_line (P_MSG, y, "", 8); continue; }
        int idx = (histIdx - k + HISTORY_ROWS) % HISTORY_ROWS;
        char buf[81];
        strncpy (buf, hist + idx * 80, 80);
        buf[80] = 0;
        text_line (P_MSG, y, buf, 7);
    }
    for (int y = 0; y < live; ++y)
        for (int x = 0; x < q->cols; ++x) {
            Cell c = { ' ', 15, 0 };
            if (x < 80) c = w->c[w->y1 + y][x];
            if (c.fg == 7) c.fg = 15; /* new messages bright */
            draw_text (P_MSG, old + y, x, c);
        }
#ifdef __EMSCRIPTEN__
    {   /* the cursor row of the log: the prompt line over the map */
        char r[81];
        int ly = w->cy - w->y1;
        if (ly < 0 || ly >= live) ly = live - 1;
        for (int x = 0; x < 80; ++x) r[x] = ly >= 0 ? (char) w->c[w->y1 + ly][x].ch : ' ';
        r[80] = 0;
        js_prompt (r);
    }
#endif
}

static void
draw_inventory ()
{
    struct pane *q = &P[P_INV];
    shCreature *h = Hero.cr ();
    int per = q->rows, colw;
    std::vector<std::string> lines;
    std::vector<int> fgs;   /* each item's own glyph colour */
    /* only once the hero stands on a level: inv () asks it about shops */
    if (heroPlaced () && h->mInventory) {
        static char save[64 * SHBUFLEN];
        int n;
        GetBufSave (save, &n);
        for (char l = 'A'; l <= 'z'; ++l)
            for (int i = 0; i < h->mInventory->count (); ++i) {
                shObject *o = h->mInventory->get (i);
                if (o->mLetter != l) continue;
                char buf[128];
                snprintf (buf, sizeof buf, "%c %s", l, o->inv ());
                lines.push_back (buf);
                fgs.push_back (o->getGlyph ().mColor);
            }
        GetBufRestore (save, n);
    }
    int ncol = (int) lines.size () > per ? 2 : 1;
    colw = q->cols / ncol;
    for (int col = 0; col < ncol; ++col)
        for (int y = 0; y < per; ++y) {
            size_t k = col * per + y;
            const char *s = k < lines.size () ? lines[k].c_str () : "";
            Cell c = { ' ', 7, 0 };
            for (int x = 0; x < colw - 1; ++x) {
                c.ch = *s ? *s++ : ' ';
                c.fg = x == 0 ? 15 : k < fgs.size () && fgs[k] ? fgs[k] : 7;
                draw_text (P_INV, y, col * colw + x, c);
            }
        }
}

/* The open pop-up windows (kTemp, kMenu, kMenuHelp), each cut to the
   bounding box of its content, stacked in the order they were opened. */

static struct PopRow { int win, row; } popRows[25 * 3 + 2];

static void
draw_popup (shXInterface *ui)
{
    struct Block { TWin *w; int y0, y1, x0, x1; } bl[3];
    int nb = 0, rows = 0, cols = 0;
    for (int seq = 1; seq <= ui->mOpenSeq; ++seq)
        for (int wi = shInterface::kTemp; wi <= shInterface::kMenuHelp; ++wi) {
            TWin *w = &ui->mW[wi];
            if (w->open != seq || nb == 3) continue;
            Block b = { w, 99, -1, 99, -1 };
            for (int y = w->y1; y < w->y2; ++y)
                for (int x = w->x1; x < w->x2; ++x)
                    if (w->c[y][x].ch != ' ' || w->c[y][x].bg) {
                        if (y < b.y0) b.y0 = y;
                        if (y > b.y1) b.y1 = y;
                        if (x < b.x0) b.x0 = x;
                        if (x > b.x1) b.x1 = x;
                    }
            if (b.y1 < 0) continue;
            if (nb) rows++; /* a blank line between blocks */
            rows += b.y1 - b.y0 + 1;
            if (b.x1 - b.x0 + 1 > cols) cols = b.x1 - b.x0 + 1;
            bl[nb++] = b;
        }
    if (!nb) { popup (0, 0); return; }
    popup (rows, cols);
    Cell blank = { ' ', 7, 0 };
    int py = 0;
    for (int i = 0; i < nb; ++i) {
        if (i) { popRows[py].win = -1; for (int x = 0; x < cols; ++x) draw_text (P_POP, py, x, blank); py++; }
        for (int y = bl[i].y0; y <= bl[i].y1; ++y, ++py) {
            popRows[py].win = bl[i].w - ui->mW;
            popRows[py].row = y - bl[i].w->y1;
            for (int x = 0; x < cols; ++x)
                draw_text (P_POP, py, x, bl[i].x0 + x <= bl[i].x1 ? bl[i].w->c[y][bl[i].x0 + x] : blank);
        }
    }
}

/* PRIME_TILEGAPS=<file>: list every monster / item glyph whose tile is
   empty or the coloured-letter fallback (for port/mktiles.py). */
static bool
tileEmpty (int tx, int ty)
{
    if (!sheet || tx < 0 || ty < 0 || (tx + 1) * TS > sheet_w || (ty + 1) * TS > sheet_h) return true;
    for (int y = 0; y < TS; ++y)
        for (int x = 0; x < TS; ++x)
            if (sheet[((size_t) (ty * TS + y) * sheet_w + tx * TS + x) * 4 + 3]) return false;
    return true;
}

static void
gap (FILE *f, const char *kind, const char *name, shGlyph *g)
{
    bool letter = (g->mTileY == 2 || (g->mTileY >= kRowLittleA && g->mTileY < kRowBigA + 26)) && g->mTileX >= 24;
    if (letter || tileEmpty (g->mTileX, g->mTileY))
        fprintf (f, "%s\t%s\t%c\t%d,%d\t%s\n", kind, name, g->mSym, g->mTileX, g->mTileY,
                 letter ? "letter" : "empty");
}

static void
tileGapReport ()
{
    extern shFlavor Flavors[];
    const char *fn = getenv ("PRIME_TILEGAPS");
    static bool done;
    FILE *f;
    if (done || !fn || !(f = fopen (fn, "w"))) return;
    done = true;
    for (int i = 0; i < kMonNumberOf; ++i)
        gap (f, "monster", MonIlks[i].mName, &MonIlks[i].mGlyph);
    for (int i = 0; i < kObjNumIlks; ++i) {
        shObjectIlk *k = &AllIlks[i];
        gap (f, "item", k->mReal.mName ? k->mReal.mName : "?", &k->mReal.mGlyph);
        if (k->mAppearance.mName && k->mAppearance.mGlyph.mTileY != k->mReal.mGlyph.mTileY + 999)
            gap (f, "item-look", k->mAppearance.mName, &k->mAppearance.mGlyph);
        if (k->mVague.mName)
            gap (f, "item-vague", k->mVague.mName, &k->mVague.mGlyph);
    }
    for (int i = 0; i < kFNumFlavors; ++i)
        if (Flavors[i].mAppearance.mName)
            gap (f, "flavor", Flavors[i].mAppearance.mName, &Flavors[i].mAppearance.mGlyph);
    fclose (f);
}

void
shXInterface::present ()
{
    if (!dpy) return;
    if (heroPlaced ()) tileGapReport ();
    draw_map ();
    draw_window_region (P_STATUS, &mW[kSide], mW[kSide].y1, mW[kSide].x1);
    draw_messages (this, mHistoryIdx, mHistoryWrapped, mLogHistory);
    draw_inventory ();
    draw_popup (this);
#ifdef __EMSCRIPTEN__
    {
        shCreature *h = Hero.cr ();
        bool target = mCurX < MAPMAXCOLUMNS && mCurY < MAPMAXROWS && h && heroPlaced ()
            && !(h->mX == mCurX && h->mY == mCurY);
        int fy = target ? mCurY : heroPlaced () ? h->mY : -1;
        int fx = target ? mCurX : heroPlaced () ? h->mX : -1;
        sendVisible ();
        js_flush (fy, fx, target ? mCurY : -1, target ? mCurX : -1);
    }
#else
    for (int p = 0; p < NPANES; p++) {
        struct pane *q = &P[p];
        if (q->win && q->pix && q->rows)
            XCopyArea (dpy, q->pix, q->win, gc, 0, 0, q->cols * q->cw + 2 * q->pad,
                       q->rows * q->ch + 2 * q->pad, 0, 0);
    }
    /* Targeting cursor: an outline on the map when it isn't on the hero. */
    shCreature *h = Hero.cr ();
    if (mCurX < MAPMAXCOLUMNS && mCurY < MAPMAXROWS && h && !(h->mX == mCurX && h->mY == mCurY)
        && mCurX >= viewX0 && mCurX < viewX0 + viewCols) {
        XSetForeground (dpy, gc, pix[14]);
        XDrawRectangle (dpy, P[P_MAP].win, gc, (mCurX - viewX0) * cw, mCurY * cw, cw - 1, cw - 1);
    }
    XFlush (dpy);
#endif
    const char *dp = getenv ("PRIME_DUMP");
    if (dp && (dumpf = fopen (dp, "w"))) {
        fprintf (dumpf, "== MAP (x0=%d)\n", viewX0);
        for (int y = 0; y < MAPMAXROWS; ++y) {
            for (int x = 0; x < MAPMAXCOLUMNS; ++x)
                fputc (mW[kMain].c[y][x].ch ? mW[kMain].c[y][x].ch : ' ', dumpf);
            fputc ('\n', dumpf);
        }
        dump_pane ("STATUS", P_STATUS);
        dump_pane ("MSG", P_MSG);
        dump_pane ("INV", P_INV);
        dump_pane ("POP", P_POP);
        fclose (dumpf);
        dumpf = NULL;
    }
}

#ifdef __EMSCRIPTEN__
int RvipAtPrompt;          /* Rvip.cpp: waiting for a command (safe to autosave) */
int PlayerSaved;           /* Hero.cpp: "Save and quit" worked */
static double lastYield;

static void webAutosave ();

/* Keys come as the codes prime.js sends (the same as keycode () above
   makes); a click on a pop-up row as 0x10000 + row. */
static int
getkey (bool wait)
{
    for (;;) {
        int k = js_key (RvipAtPrompt);
        if (k >= 0x10000) {
            int r = k - 0x10000;
            if (r < P[P_POP].rows && popRows[r].win >= 0) {
                UIClickWin = popRows[r].win;
                UIClickRow = popRows[r].row;
                return KEY_CLICK;
            }
            continue;
        }
        if (k >= 0) return k;
        if (RvipAtPrompt && js_want_save ()) webAutosave ();
        if (!wait) return -1;
        emscripten_sleep (10);
        lastYield = emscripten_get_now ();
    }
}

/* Explore asks before every step: let the page paint now and then. */
bool
x11KeyPending ()
{
    if (emscripten_get_now () - lastYield > 40) {
        emscripten_sleep (0);
        lastYield = emscripten_get_now ();
    }
    return EM_ASM_INT ({ return Module.pr.pending (); });
}

/* PRIME deletes the save it loads: keep a copy while the game runs
   (temp file + rename, so a reload mid-write can't lose it). */
static void
webAutosave ()
{
    extern char UserDir[];
    char path[256], tmp[256];
    if (!heroPlaced () || GameOver) return;
    snprintf (path, sizeof path, "%s/save/%s.sav", UserDir, Hero.cr ()->mName);
    snprintf (tmp, sizeof tmp, "%s.tmp", path);
    unlink (tmp);
    if (0 == saveGame (tmp)) rename (tmp, path);
    EM_ASM ({ Module.pr.saved (); });
}

/* exitPRIME (): the game is over or saved. Without a real save the
   autosave must go, or a dead character comes back. */
void
webEnd ()
{
    extern char UserDir[];
    char path[256];
    if (!PlayerSaved && Hero.cr ()) {
        snprintf (path, sizeof path, "%s/save/%s.sav", UserDir, Hero.cr ()->mName);
        unlink (path);
    }
    js_end (PlayerSaved);
}
#else
/* Arrows and Home/End/PgUp/PgDn are the keymap's named keys; the keypad
   sends digits and + - * / . (RVIP numpad rules). */
static int
keycode (XKeyEvent *ev)
{
    char buf[8];
    KeySym ks;
    int n = XLookupString (ev, buf, sizeof buf, &ks, NULL);
    switch (ks) {
    case XK_Left: return XK_LEFT;
    case XK_Right: return XK_RIGHT;
    case XK_Up: return XK_UP;
    case XK_Down: return XK_DOWN;
    case XK_Home: return XK_HOME;
    case XK_End: return XK_END;
    case XK_Prior: return XK_PGUP;
    case XK_Next: return XK_PGDN;
    case XK_Insert: return XK_INS;
    case XK_Delete: return XK_DEL;
    case XK_KP_Left: case XK_KP_4: return '4';
    case XK_KP_Right: case XK_KP_6: return '6';
    case XK_KP_Up: case XK_KP_8: return '8';
    case XK_KP_Down: case XK_KP_2: return '2';
    case XK_KP_Home: case XK_KP_7: return '7';
    case XK_KP_Prior: case XK_KP_9: return '9';
    case XK_KP_End: case XK_KP_1: return '1';
    case XK_KP_Next: case XK_KP_3: return '3';
    case XK_KP_Begin: case XK_KP_5: return '5';
    case XK_KP_Enter: case XK_Return: return 13; /* Ctrl+J stays 10 */
    case XK_BackSpace: return 8;
    case XK_KP_Add: return '+';
    case XK_KP_Subtract: return '-';
    case XK_KP_Multiply: return '*';
    case XK_KP_Divide: return '/';
    case XK_KP_Decimal: case XK_KP_Delete: return '.';
    case XK_KP_0: case XK_KP_Insert: return '0';
    }
    if (ks >= XK_F1 && ks <= XK_F12) return XK_F0 + 1 + (ks - XK_F1);
    if (n == 1) return (unsigned char) buf[0];
    return -1;
}

static int
getkey (bool wait)
{
    XEvent ev;
    for (;;) {
        if (!wait && !XPending (dpy)) return -1;
        XNextEvent (dpy, &ev);
        if (ev.type == Expose) {
            for (int p = 0; p < NPANES; ++p)
                if (P[p].win == ev.xexpose.window && P[p].pix)
                    XCopyArea (dpy, P[p].pix, P[p].win, gc, 0, 0, P[p].cols * P[p].cw + 2 * P[p].pad,
                               P[p].rows * P[p].ch + 2 * P[p].pad, 0, 0);
        } else if (ev.type == KeyPress) {
            int k = keycode (&ev.xkey);
            if (k >= 0) return k;
        } else if (ev.type == ButtonPress && ev.xbutton.button == Button1
                   && ev.xbutton.window == P[P_POP].win && P[P_POP].rows) {
            int r = (ev.xbutton.y - P[P_POP].pad) / P[P_POP].ch;
            if (r >= 0 && r < P[P_POP].rows && popRows[r].win >= 0) {
                UIClickWin = popRows[r].win;
                UIClickRow = popRows[r].row;
                return KEY_CLICK;
            }
        }
    }
}

/* True when a key is waiting (explore stops on it). */
bool
x11KeyPending ()
{
    if (!dpy) return false;
    while (XPending (dpy)) {
        XEvent ev;
        XPeekEvent (dpy, &ev);
        if (ev.type == KeyPress) return true;
        XNextEvent (dpy, &ev);
        if (ev.type == Expose)
            for (int p = 0; p < NPANES; ++p)
                if (P[p].win == ev.xexpose.window && P[p].pix)
                    XCopyArea (dpy, P[p].pix, P[p].win, gc, 0, 0, P[p].cols * P[p].cw + 2 * P[p].pad,
                               P[p].rows * P[p].ch + 2 * P[p].pad, 0, 0);
    }
    return false;
}

#endif

/**********************************************************************
 * The interface
 */

shXInterface::shXInterface ()
{
    memset (mW, 0, sizeof mW);
    for (int i = 0; i < kMaxWin; ++i) {
        mW[i].x1 = 0; mW[i].y1 = 0; mW[i].x2 = 80; mW[i].y2 = 25;
        for (int y = 0; y < 25; ++y)
            for (int x = 0; x < 80; ++x) {
                mW[i].c[y][x].ch = ' '; mW[i].c[y][x].fg = 7; mW[i].c[y][x].bg = 0;
            }
    }
    mW[kMain].x2 = 64; mW[kMain].y2 = 20;
    mW[kSide].x1 = 64; mW[kSide].x2 = 80; mW[kSide].y2 = 20;
    mW[kLog].y1 = 20; mW[kLog].y2 = 25;
    for (int i = 0; i < kMaxWin; ++i) { mW[i].cx = mW[i].x1; mW[i].cy = mW[i].y1; }
    mW[kMain].open = mW[kSide].open = mW[kLog].open = 0;
    mOpenSeq = 0;
    mFg = kGray; mBg = kBlack;
    mCurX = mCurY = 99;
    mCursType = 0;
    for (int y = 0; y < MAPMAXROWS; ++y)
        for (int x = 0; x < MAPMAXCOLUMNS; ++x)
            mCache[x][y].needs_update = true;

    mColor = kGray;
    mLogSize = 5;
    mLogRow = 0;
    mHistoryIdx = 0;
    mHistoryWrapped = 0;
    mNoNewline = 0;
    mLogSCount = 0;
    mPause = 0;
    memset (mLogHistory, 0, sizeof mLogHistory);

    open_display ();
    pane_init (P_MAP, viewCols, MAPMAXROWS);
    pane_init (P_STATUS, 16, MAPMAXROWS);
    pane_init (P_MSG, 80, 11);
#ifdef __EMSCRIPTEN__
    pane_init (P_INV, 40, 26);  /* the page's layout: inventory under Status */
#else
    {
        int x, y;
        place (P_INV, &x, &y);
        int cols = (1440 - x - tw) / tw;
        pane_init (P_INV, cols, 11);
    }
    XFlush (dpy);
#endif
}

shXInterface::~shXInterface ()
{
#ifndef __EMSCRIPTEN__
    if (dpy) XCloseDisplay (dpy);
    dpy = NULL;
#endif
}

void
shXInterface::newWin (Window win)
{
    mW[win].open = ++mOpenSeq;
    mW[win].x1 = 0; mW[win].y1 = 0; mW[win].x2 = 80; mW[win].y2 = 25;
    mW[win].cx = mW[win].cy = 0;
    clearWin (win);
}

void
shXInterface::delWin (Window win)
{
    clearWin (win);
    mW[win].open = 0;
    int top = 0;
    for (int i = kTemp; i <= kMenuHelp; ++i)
        if (mW[i].open > top) top = mW[i].open;
    mOpenSeq = top;
    if (heroPlaced ()) drawScreen ();
    else present ();
}

void
shXInterface::moveWin (Window win, int x1, int y1, int x2, int y2)
{
    mW[win].x1 = x1; mW[win].x2 = x2 > 80 ? 80 : x2;
    mW[win].y1 = y1; mW[win].y2 = y2 > 25 ? 25 : y2;
    mW[win].cx = x1; mW[win].cy = y1;
}

void
shXInterface::clearWin (Window win)
{
    TWin *w = &mW[win];
    for (int y = w->y1; y < w->y2; ++y)
        for (int x = w->x1; x < w->x2; ++x) {
            w->c[y][x].ch = ' '; w->c[y][x].fg = 7; w->c[y][x].bg = 0;
        }
}

void
shXInterface::setWinColor (Window, shColor fg, shColor bg)
{
    mFg = fg;
    mBg = bg;
}

void
shXInterface::winGoToYX (Window win, int y, int x)
{
    mW[win].cy = mW[win].y1 + y;
    mW[win].cx = mW[win].x1 + x;
}

void
shXInterface::winGetYX (Window win, int *y, int *x)
{
    *y = mW[win].cy - mW[win].y1;
    *x = mW[win].cx - mW[win].x1;
}

void
shXInterface::put (Window win, char ch)
{
    TWin *w = &mW[win];
    if (ch == '\n') {
        w->cx = w->x1;
        if (++w->cy >= w->y2 && win == kLog) { /* scroll the log */
            for (int y = w->y1; y < w->y2 - 1; ++y)
                memcpy (w->c[y], w->c[y + 1], sizeof w->c[y]);
            for (int x = 0; x < 80; ++x) {
                w->c[w->y2 - 1][x].ch = ' '; w->c[w->y2 - 1][x].fg = 7; w->c[w->y2 - 1][x].bg = 0;
            }
            w->cy = w->y2 - 1;
        }
        return;
    }
    if (w->cy >= 0 && w->cy < 25 && w->cx >= 0 && w->cx < 80) {
        w->c[w->cy][w->cx].ch = ch;
        w->c[w->cy][w->cx].fg = mFg;
        w->c[w->cy][w->cx].bg = mBg;
    }
    ++w->cx;
}

void
shXInterface::winPutchar (Window win, const char c)
{
    put (win, c);
}

void
shXInterface::winPrint (Window win, const char *fmt, ...)
{
    va_list ap;
    char linebuf[256];
    va_start (ap, fmt);
    vsnprintf (linebuf, sizeof linebuf, fmt, ap);
    va_end (ap);
    for (char *s = linebuf; *s; ++s) put (win, *s);
}

void
shXInterface::winPrint (Window win, int y, int x, const char *fmt, ...)
{
    va_list ap;
    char linebuf[256];
    va_start (ap, fmt);
    vsnprintf (linebuf, sizeof linebuf, fmt, ap);
    va_end (ap);
    winGoToYX (win, y, x);
    for (char *s = linebuf; *s; ++s) put (win, *s);
}

void
shXInterface::mapGlyph (int x, int y, shGlyph g)
{
    if (x < 0 || y < 0 || x >= 64 || y >= 20) return;
    mW[kMain].c[y][x].ch = g.mSym;
    mW[kMain].c[y][x].fg = g.mColor;
    mW[kMain].c[y][x].bg = g.mBkgd;
}

static const struct shNameToKey KeyNames[] =
{
    {"space", ' '},
    {"tab", '\t'},
    {"backspace", 8},
    {"enter", 13},
    {"escape", 27},
    {"down arrow", XK_DOWN},
    {"up arrow", XK_UP},
    {"left arrow", XK_LEFT},
    {"right arrow", XK_RIGHT},
    {"keypad 1", XK_END},
    {"keypad 2", XK_DOWN},
    {"keypad 3", XK_PGDN},
    {"keypad 4", XK_LEFT},
    {"keypad 5", XK_CENTER},
    {"keypad 6", XK_RIGHT},
    {"keypad 7", XK_HOME},
    {"keypad 8", XK_UP},
    {"keypad 9", XK_PGUP},
    {"home", XK_HOME},
    {"end", XK_END},
    {"insert", XK_INS},
    {"delete", XK_DEL},
    {"page up", XK_PGUP},
    {"page down", XK_PGDN},
    {"fn 1", XK_F0 + 1},
    {"fn 2", XK_F0 + 2},
    {"fn 3", XK_F0 + 3},
    {"fn 4", XK_F0 + 4},
    {"fn 5", XK_F0 + 5},
    {"fn 6", XK_F0 + 6},
    {"fn 7", XK_F0 + 7},
    {"fn 8", XK_F0 + 8},
    {"fn 9", XK_F0 + 9},
    {"fn 10", XK_F0 + 10},
    {"fn 11", XK_F0 + 11},
    {"fn 12", XK_F0 + 12}
};
static const int n_keys = sizeof (KeyNames) / sizeof (shNameToKey);

void
shXInterface::readKeybindings (const char *fname)
{
    I->readKeybindings (fname, n_keys, KeyNames);
}

static const char *
keyCodeToString (int code)
{
    if (code >= XK_F0 + 1 and code <= XK_F0 + 12) {
        char *buf = GetBuf ();
        sprintf (buf, "F%d", code - XK_F0);
        return buf;
    }
    for (int i = 0; i < n_keys; ++i)
        if (KeyNames[i].key == code)
            return KeyNames[i].name;
    char *buf = GetBuf ();
    if (code < ' ')
        sprintf (buf, "ctrl %c", code + 64);
    else
        sprintf (buf, "%c", code);
    return buf;
}

const char *
shXInterface::getKeyForCommand (Command cmd)
{
    int i;
    for (i = 0; i < XK_MAXKEY and cmd != mKey2Cmd[i]; ++i)
        ;
    if (i == XK_MAXKEY)
        return NULL;
    return keyCodeToString (i);
}

shVector<const char *> *
shXInterface::getKeysForCommand (Command cmd)
{
    shVector<const char *> *keys = new shVector<const char *> ();
    for (int i = 0; i < XK_MAXKEY; ++i)
        if (cmd == mKey2Cmd[i])  keys->add (keyCodeToString (i));
    return keys;
}

static int pushedKey = -1;

void
x11PushKey (int key)
{
    pushedKey = key;
}

int
shXInterface::getChar ()
{
    if (pushedKey >= 0) {
        int k = pushedKey;
        pushedKey = -1;
        return k;
    }
    present ();
    return getkey (true);
}

int
shXInterface::getSpecialChar (SpecialKey *sk)
{
    int key = getChar ();
    switch (key) {
    case 27: *sk = kEscape; break;
    case 10: case 13: *sk = kEnter; break;
    case ' ': *sk = kSpace; break;
    case 8: *sk = kBackSpace; break;
    case XK_HOME: *sk = kHome; break;
    case XK_END: *sk = kEnd; break;
    case XK_PGUP: *sk = kPgUp; break;
    case XK_PGDN: *sk = kPgDn; break;
    case XK_UP: *sk = kUpArrow; break;
    case XK_DOWN: *sk = kDownArrow; break;
    case XK_LEFT: *sk = kLeftArrow; break;
    case XK_RIGHT: *sk = kRightArrow; break;
    case XK_CENTER: *sk = kCenter; break;
    case XK_INS: *sk = kInsert; break;
    case XK_DEL: *sk = kDelete; break;
    default: *sk = kNoSpecialKey; break;
    }
    return key;
}

int
shXInterface::getStr (char *buf, int len, const char *prompt, const char *dflt)
{
    char msg[80];
    int pos = 0;
    int savehistidx = mHistoryIdx;

    snprintf (msg, 80, "%s ", prompt);
    msg[79] = 0;
    p (msg);
    buf[0] = 0;

    if (dflt) {
        strncpy (buf, dflt, len);
        buf[len-1] = 0;
        winPrint (kLog, "%s", buf);
        pos = strlen (buf);
    }

    while (1) {
        int c = getChar ();
        if (c < 0x100 && isprint (c)) {
            if (pos >= len - 1)
                continue;
            buf[pos++] = c;
            winPutchar (kLog, c);
        } else if ('\n' == c or '\r' == c) {
            break;
        } else if (8 == c and pos) {
            pos--;
            int y, x;
            winGetYX (kLog, &y, &x);
            winPrint (kLog, y, x - 1, " ");
            winGoToYX (kLog, y, x - 1);
        } else if (27 == c) {
            pos = 0;
            break;
        }
    }

    buf[pos] = 0;
    snprintf (msg, 80, "%s %s", prompt, buf);
    msg[79] = 0;
    strcpy (&mLogHistory[savehistidx*80], msg);
    return pos;
}

int
shXInterface::diag (const char *format, ...)
{
    va_list ap;
    va_start (ap, format);
    char dbgbuf[100];
    vsnprintf (dbgbuf, 100, format, ap);
    debug.log ("%s", dbgbuf);
    va_end (ap);
    return 0;
}

void
shXInterface::cursorOnXY (int x, int y, int curstype)
{
    static bool curson = false;
    static int crx, cry;
    if (curson) {
        mCache[crx][cry].dellast ();
        mCache[crx][cry].needs_update = true;
        curson = false;
    }
    if (curstype && x < MAPMAXCOLUMNS && y < MAPMAXROWS) {
        curson = true;
        mCache[x][y].add (curstype, kRowCursor, 0);
        mCache[x][y].needs_update = true;
        crx = x;  cry = y;
    }
    mCurX = x;
    mCurY = y;
}

void
shXInterface::pageLog ()
{
    mLogSCount = 0;
    mLogRow = 0;
    clearWin (kLog);
    winGoToYX (kLog, 0, 0);
}

void
shXInterface::doScreenShot (FILE *file)
{
    for (int y = 0; y < 20; ++y) {
        char line[81];
        for (int x = 0; x < 80; ++x)
            line[x] = x < 64 ? mW[kMain].c[y][x].ch : mW[kSide].c[y][x].ch;
        line[80] = 0;
        fprintf (file, "%s\n", line);
    }
    for (int y = mLogSCount - 1; y >= 0; --y) {
        int idx = (mHistoryIdx - y - 1 + HISTORY_ROWS) % HISTORY_ROWS;
        fprintf (file, "%s\n", &mLogHistory[idx * 80]);
    }
}

void
shXInterface::showVersion ()
{
    const int BX = 11, EX = 23;
    const int BY = 44, EY = 50;

    for (int x = 0; x < MAPMAXCOLUMNS; ++x)
        for (int y = 0; y < MAPMAXROWS; ++y)
            mCache[x][y].reset ();
    /* The logo, centred in the map view. */
    int ox = (viewCols - (EX - BX + 1)) / 2, oy = (MAPMAXROWS - (EY - BY + 1)) / 2;
    if (ox < 0) ox = 0;
    for (int x = BX; x <= EX; ++x)
        for (int y = BY; y <= EY; ++y)
            mCache[x - BX + ox][y - BY + oy].add (x, y, 0);
    mCurX = viewCols / 2;

    I->p ("PRIME " PRIME_VERSION " - Cosmic Drifter");
    I->p ("Unofficial variant of ZAPM by Psiweapon and Michal Bielinski.");
}

void
shXInterface::hideVersion ()
{
    for (int x = 0; x < MAPMAXCOLUMNS; ++x)
        for (int y = 0; y < MAPMAXROWS; ++y)
            mCache[x][y].reset ();
    pageLog ();
    mCurX = 99;
}

void
shXInterface::drawScreen ()
{
    if (!heroPlaced ()) { present (); return; }
    Level->draw ();
    drawSideWin (Hero.cr ());
    present ();
}

void
shXInterface::drawLog ()
{
    present ();
}

void
shXInterface::refreshScreen ()
{
    present ();
}

/**********************************************************************
 * Tile stacks: from NEUI.cpp (NotEye frontend), unchanged except that the
 * ASCII glyph goes to the kMain text grid (dumps, screenshots).
 */

const int Floor = spFlat + spFloor + spIFloor + spCeil;
const int spWall = spWallN + spWallE + spWallS + spWallW;
const int Wall = spFlat + spWall + spIWallL + spIWallR + spICeil;
const int spFeature = spFlat + spMonst + spIItem;
const int WayDown = spFlat + spFloor + spIFloor;

static void
drawSpecialEffect (eff::type effect, shCache *cache)
{
    const int OPT = 23; /* Difference between laser and optic blast. */
    const int eff2tile[eff::last_corner][2] =
    {   /* None: */ {5, kRowDefault},
        /* Laser: f-slash, horiz, vert, b-slash, hit. */
        {1, kRow4DirAtt0}, {2, kRow4DirAtt0}, {3, kRow4DirAtt0},
        {4, kRow4DirAtt0}, {5, kRow4DirAtt0},
        /* Optic blast: f-slash, horiz, vert, b-slash, hit. */
        {1+OPT, kRow4DirAtt0}, {2+OPT, kRow4DirAtt0}, {3+OPT, kRow4DirAtt0},
        {4+OPT, kRow4DirAtt0}, {5+OPT, kRow4DirAtt0},
        /* Bolt: f-slash, horiz, vert, b-slash, hit. */
        {10, kRow4DirAtt0}, {11, kRow4DirAtt0}, {12, kRow4DirAtt0},
        {13, kRow4DirAtt0}, {14, kRow4DirAtt0},
        /* Packet storm: SW-NE, E-W, N-S, SE-NW, hit. */
        {15, kRow4DirAtt0}, {16, kRow4DirAtt0}, {17, kRow4DirAtt0},
        {18, kRow4DirAtt0}, {19, kRow4DirAtt0},
        /* Heat ray: SW-NE, E-W, N-S, SE-NW, hit. */
        {10, kRow4DirAtt1}, {11, kRow4DirAtt1}, {12, kRow4DirAtt1},
        {13, kRow4DirAtt1}, {14, kRow4DirAtt1},
        /* Antimatter ray: SW-NE, E-W, N-S, SE-NW, hit. */
        {1, kRow4DirAtt1}, {2, kRow4DirAtt1}, {3, kRow4DirAtt1},
        {4, kRow4DirAtt1}, {5, kRow4DirAtt1},
        /* Railgun: N, NE, E, SE, S, SW, W, NW directions and hit. */
        {3, kRow8DirAtt0}, {1, kRow8DirAtt0}, {6, kRow8DirAtt0},
        {8, kRow8DirAtt0}, {7, kRow8DirAtt0}, {5, kRow8DirAtt0},
        {2, kRow8DirAtt0}, {4, kRow8DirAtt0}, {5, kRowDefault},
        /* Psi vomit: N, NE, E, SE, S, SW, W, NW directions and hit. */
        {23, kRow8DirAtt0}, {21, kRow8DirAtt0}, {26, kRow8DirAtt0},
        {28, kRow8DirAtt0}, {27, kRow8DirAtt0}, {25, kRow8DirAtt0},
        {22, kRow8DirAtt0}, {24, kRow8DirAtt0}, {25, kRowDefault},
        /* Hydralisk: N, NE, E, SE, S, SW, W, NW directions and hit. */
        {3, kRow8DirAtt1}, {1, kRow8DirAtt1}, {6, kRow8DirAtt1},
        {8, kRow8DirAtt1}, {7, kRow8DirAtt1}, {5, kRow8DirAtt1},
        {2, kRow8DirAtt1}, {4, kRow8DirAtt1}, {9, kRow8DirAtt1},
        /* Combi-stick: N, NE, E, SE, S, SW, W, NW, E-W, N-S, SW-NE, SE-NW. */
        {11, kRow8DirAtt0}, { 9, kRow8DirAtt0}, {14, kRow8DirAtt0},
        {16, kRow8DirAtt0}, {15, kRow8DirAtt0}, {13, kRow8DirAtt0},
        {10, kRow8DirAtt0}, {12, kRow8DirAtt0}, {18, kRow8DirAtt0},
        {19, kRow8DirAtt0}, {17, kRow8DirAtt0}, {20, kRow8DirAtt0},
        /* NNTP Daemon flame breath. */
        { 7, kRow0DirAtt}, { 8, kRow0DirAtt}, { 9, kRow0DirAtt},
        {10, kRow0DirAtt}, {11, kRow0DirAtt}, {12, kRow0DirAtt},
        {13, kRow0DirAtt}, {14, kRow0DirAtt},
        /* Invis (should not be used), pea pellet, plasma glob, plasma hit. */
        {5, kRowDefault}, {1, kRow0DirAtt}, {2, kRow0DirAtt}, {3, kRowBoom},
        /* Explosion, frost, poison, radiation. */
        {1, kRowBoom}, {4, kRow0DirAtt}, {6, kRow0DirAtt}, {2, kRowBoom},
        /* Flashbang, incendiary, psi storm, disintegration. */
        {4, kRowBoom}, {5, kRow0DirAtt}, {6, kRowBoom}, {3, kRow0DirAtt},
        /* Shrapnel/frag, acid splash, defiler vomit, water splash. */
        {5, kRowBoom}, {2, kRowSplash}, {1, kRowBreath}, {5, kRowSplash},
        /* Web, Bugs, viruses. */
        {8, kRowBoom}, {5, kRowDefault}, {6, kRow0DirAtt},
        /* Smart-Disc, radar blip, sensed life, sensed tremor. */
        {5, kRowMissile}, {4, kRowCursor}, {5, kRowCursor}, {6, kRowCursor},
        /* last_effect: */ {5, kRowDefault},
        /* Laser beam corners: NW, NE, SE, SE. */
        {6, kRow4DirAtt0}, {7, kRow4DirAtt0}, {8, kRow4DirAtt0},
        {9, kRow4DirAtt0},
        /* Optic blast corners: NW, NE, SE, SE. */
        {6+OPT, kRow4DirAtt0}, {7+OPT, kRow4DirAtt0}, {8+OPT, kRow4DirAtt0},
        {9+OPT, kRow4DirAtt0},
        /* Psionic vomit corners: NW, NE, SE, SE. */
        {30, kRow8DirAtt0}, {31, kRow8DirAtt0}, {32, kRow8DirAtt0},
        {33, kRow8DirAtt0},
        /* Packet storm corners: NW, NE, SE, SE. */
        {20, kRow4DirAtt0}, {21, kRow4DirAtt0}, {22, kRow4DirAtt0},
        {23, kRow4DirAtt0},
        /* Heat ray corners: NW, NE, SE, SE. */
        {15, kRow4DirAtt1}, {16, kRow4DirAtt1}, {17, kRow4DirAtt1},
        {18, kRow4DirAtt1},
        /* Antimatter ray corners: NW, NE, SE, SE. */
        {6, kRow4DirAtt1}, {7, kRow4DirAtt1}, {8, kRow4DirAtt1},
        {9, kRow4DirAtt1}
    };
    /* Stored here as documentation.  Splashes row: */
    /* Coffee, BBB/Acid, Beer, Cola, Water, Sludge, Other, Blood */

    const int Bolt = spFlat + spFree + spIItem;
    const int BoltHit = spFlat + spMonst + spIItem;
    const int Marker = spFlat + spIItem;

    switch (effect) {
    /* Never drawn effects. */
    case eff::none:  case eff::invis:  case eff::rail:
        return;
    /* Here go effects without tiles at the moment.  Don't draw anything. */
    case eff::bugs:
        return;
    /* Hit tiles in bolt row. */
    case eff::laser:  case eff::optic:  case eff::bolt:  case eff::psi_vomit:
        cache->add (eff2tile[effect][0], eff2tile[effect][1], BoltHit);
        return;
    default:
        break;
    }
    int col = eff2tile[effect][0];
    int row = eff2tile[effect][1];
    switch (row) {
    case kRow8DirAtt0:  case kRow8DirAtt1:
    case kRow4DirAtt0:  case kRow4DirAtt1:
        cache->add (col, row, Bolt);
        return;
    case kRow0DirAtt:  case kRowSplash:  case kRowBoom:
        cache->add (col, row, BoltHit);
        return;
    case kRowCursor:  case kRowMissile:
        cache->add (col, row, Marker);
        return;
    }
}


void
shXInterface::terr2tile (shTerrainType terr, shCache *cache, int odd, int recolor)
{
    switch (terr) {
    case kStone:
        break;
    case kSewerWall1:
        cache->add (1, kRowSewers, Wall, recolor);
        break;
    case kSewerWall2:
        cache->add (2, kRowSewers, Wall, recolor);
        break;
    case kCavernWall1:
        cache->add (1, kRowGammaCaves, Wall, recolor);
        break;
    case kCavernWall2:
        cache->add (2, kRowGammaCaves, Wall, recolor);
        break;
    case kVWall: case kHWall: case kNTee: case kSTee: case kWTee: case kETee:
        cache->add (1, kRowSpaceBase, Wall, recolor);
        break;
    case kNWCorner: case kNECorner: case kSWCorner: case kSECorner:
        cache->add (2, kRowSpaceBase, spFlat + spWall + spIWallL + spIWallR, recolor);
        cache->add (1, kRowSpaceBase, spICeil, recolor);
        break;
    case kVirtualWall1:
        cache->add (1, kRowMainframe, Wall, recolor);
        break;
    case kVirtualWall2:
        cache->add (2, kRowMainframe, Wall, recolor);
        break;
    case kFloor:
    case kBrokenLightAbove:
        cache->add (5, kRowSpaceBase, Floor, recolor);
        break;
    case kCavernFloor:
        cache->add (5, kRowGammaCaves, Floor, recolor);
        break;
    case kSewerFloor:
        cache->add (5, kRowSewers, Floor, recolor);
        break;
    case kVirtualFloor:
        cache->add (5, kRowMainframe, Floor, recolor);
        break;
    case kSewage:
        cache->add (35 + odd, kRowSewers, Floor, recolor);
        break;
    case kGlassPanel:
        cache->add (5, kRowSpaceBase, Floor, recolor);
        cache->add (4, kRowSpaceBase, Wall, recolor);
        break;
    case kVoid:
    case kMaxTerrainType:
        break;
    }
}


void
shXInterface::feat2tile (shFeature *feat, shCache *cache, int recolor)
{
    int featrow = kRowASCII;
    switch (Level->mMapType) {
    case shMapLevel::kBunkerRooms:
    case shMapLevel::kTown:
    case shMapLevel::kRabbit:
        featrow = kRowSpaceBase; break;
    case shMapLevel::kSewer:
    case shMapLevel::kSewerPlant:
        featrow = kRowSewers; break;
    case shMapLevel::kRadiationCave:
        featrow = kRowGammaCaves; break;
    case shMapLevel::kMainframe:
        featrow = kRowMainframe; break;
    case shMapLevel::kTest:
        featrow = kRowASCII; break;
    }
    switch (feat->mType) {
    case shFeature::kDoorHiddenVert:
    case shFeature::kDoorHiddenHoriz:
        cache->add (1, featrow, Wall, recolor);
        break;
    case shFeature::kDoorClosed:
    case shFeature::kDoorOpen:
    {   /* Doors are very detailed. */
        int spDoor = spFlat + spCenter;
        spDoor += (feat->isHorizontalDoor ())
            ? spIWallL + spWallN + spWallS
            : spIWallR + spWallE + spWallW;
        /* Order is: closed, open, -, -, berserk closed, berserk open. */
        int x = 11; /* 11 is "plain" closed door. */
        if (feat->isOpenDoor ())  ++x;
        if (feat->isBerserkDoor () and !feat->mTrapUnknown)  x += 4;
        cache->add (x, featrow, spDoor, recolor);
        if (feat->isClosedDoor ()) { /* Show ceiling only if closed. */
            cache->add (1, featrow, spICeil, recolor);
            if (feat->isMagneticallySealed ()) {
                cache->add (14, featrow, spDoor, recolor); /* Indicate force field. */
            }
            if (feat->isInvertedDoor ()) { /* Troll face. */
                cache->add (17, featrow, spDoor, recolor);
            }
        }
        if (feat->isAutomaticDoor ()) { /* Pictures a detector above. */
            cache->add (13, featrow, spDoor, recolor);
        }
        if (feat->isClosedDoor () and feat->isLockDoor ()) {
            x = 19; /* No lock. */
            if (feat->isRetinaDoor ()) {
                x = 30;
            } else { /* Order is: open, closed, broken open, broken closed. */
                x = 26; /* 26 is "plain" open. */
                if (feat->isLockedDoor ())  ++x;
                if (feat->isLockBrokenDoor ())  x += 2;
            }
            cache->add (x, featrow, spDoor, recolor);
        }
        if (feat->isCodeLockDoor ()) { /* Color markings. */
            shObjectIlk *ilk = feat->keyNeededForDoor ();
            if (ilk)  switch (ilk->mAppearance.mGlyph.mColor) {
            case kNavy: x = 20; break;
            case kGreen: x = 21; break;
            case kRed: x = 22; break;
            case kOrange: x = 23; break;
            case kMagenta: x = 24; break;
            default: x = 25; break;
            } else {
                x = 25;
            }
            cache->add (x, featrow, spDoor, recolor);
        }
    }
    break;
    case shFeature::kStairsUp:
        cache->add (10, featrow, spFeature, recolor);
        break;
    case shFeature::kStairsDown:
        /* Most areas have way down in the floor. */
        if (featrow != kRowMainframe)
            cache->add (9, featrow, WayDown, recolor);
        else /* Sight obstructing way down. */
            cache->add (9, featrow, spFeature, recolor);
        break;
    case shFeature::kVat:
        cache->add (31, featrow, spFeature, recolor);
        break;
    case shFeature::kMovingHWall:
        cache->add (34, featrow, spFeature, recolor);
        break;
    case shFeature::kMachinery:
        cache->add (34, featrow, spFeature, recolor);
        break;
    case shFeature::kRadTrap:
        cache->add (2, kRowTrap, spFeature, recolor);
        break;
    case shFeature::kPit:
        cache->add (feat->mTrapMonUnknown == 2 ? 6 : 3, kRowTrap, spFeature, recolor);
        break;
    case shFeature::kAcidPit:
        cache->add (feat->mTrapMonUnknown == 2 ? 6 : 4, kRowTrap, spFeature, recolor);
        break;
    case shFeature::kHole:
        cache->add (5, kRowTrap, spFeature, recolor);
        break;
    case shFeature::kFloorGrating:
        cache->add (38, kRowSpaceBase, spFeature, recolor);
        break;
    case shFeature::kBrokenGrating:
        cache->add (39, kRowSpaceBase, spFeature, recolor);
        break;
    case shFeature::kSewagePit:
        cache->dellast (); /* Remove sludge. */
        cache->add (7, kRowTrap, spFeature, recolor);
        break;
    /* RVIP: the sheet's own web and hole instead of a '^'. */
    case shFeature::kWeb:
        cache->add (8, kRowBoom, spFeature, recolor);
        break;
    case shFeature::kTrapDoor:
    case shFeature::kPortableHole:
        cache->add (5, kRowTrap, spFeature, recolor);
        break;
    case shFeature::kACMESign:
    case shFeature::kPortal:
        cache->add (0, kRowTrap, spFeature, recolor);
        break;
    case shFeature::kComputerTerminal:
    case shFeature::kMaxFeatureType:
        break;
    }
}

const int spObj = spFlat + spItem + spIItem;
const int spCre = spFlat + spMonst + spIItem;

static void
putOverlay (shObject *obj, shCache *cache)
{
    using namespace obj;
    shGlyph *g = NULL;
    if (obj->has_subtype (computer)) {
        /* Show virus or antivirus status in lower right corner. */
        if (obj->is (fooproof | known_fooproof)) {
            cache->add (22, kRowTag1, spObj);
        } else if (obj->is (known_infected)) {
            cache->add (obj->is (infected) ? 23 : 37, kRowTag1, spObj);
        }
    } else if (obj->isA (kFloppyDisk)) {
        /* Add label to floppy disks. */
        if (obj->is (known_type)) {
            g = &obj->myIlk ()->mReal.mGlyph;
        } else if (obj->is (known_appearance)) {
            g = &obj->myIlk ()->mAppearance.mGlyph;
        }
        if (g) cache->add (g->mTileX, g->mTileY, spObj);
        /* Show virus status. */
        if (obj->is (known_infected)) {
            cache->add (obj->is (infected) ? 23 : 37, kRowTag1, spObj);
        }
    } else if (obj->myIlk ()->has_mini_icon () and obj->is (known_type)) {
        /* Indicate item type in upper right corner. */
        g = &obj->myIlk ()->mReal.mGlyph;
        cache->add (g->mTileX, g->mTileY, spObj);
    } else if (obj->has_subtype (jumpsuit)) {
        /* Put question mark on unidentified jumpsuit types. */
        g = &obj->myIlk ()->mReal.mGlyph;
        cache->add (obj->is (known_type) ? g->mTileX : 7, g->mTileY, spObj);
    }
    /*else if ((obj->isA (kArmor) or obj->isA (kWeapon)) and obj->is (fooproof))
    {
        cache->add (21, kRowCursor, spObj);
    }*/
}

/* Necklace of the Eye needs to be told both what tiles reside at given */
void         /* (x, y) coordinate pair and what ASCII glyph to display. */
shXInterface::draw (int x, int y, shCreature *c,
                   shObjectIlk *oi, shObject *o, shFeature *f, shSquare *s)
{
    eff::type e = Level->mEffects[x][y];
    shGlyph g = getASCII (x, y, e, c, oi, o, f, s);
    mapGlyph (x, y, g);

    shMapLevel::shMemory mem = Level->getMemory (x, y);
    mCache[x][y].needs_update = true;
    mCache[x][y].reset ();
    int recolor = Hero.cr ()->usesPower (kGammaSight) and
                  Level->isRadioactive (x, y);
    if (recolor)  recolor = 0x00FF00;
    if (s) {
        terr2tile (s->mTerr, &mCache[x][y], (x + y) % 2, recolor);
    }
    if (f) {
        feat2tile (f, &mCache[x][y], recolor);
    }
    if (o or oi) {
        shGlyph g = o ? o->getGlyph () : oi->mVague.mGlyph;
        mCache[x][y].add (g.mTileX, g.mTileY, spObj);
        if (o)  putOverlay (o, &mCache[x][y]);
    }
    if (c) {
        bool frozen = c->is (kFrozen);
        recolor = frozen ? 0x1CFFFF : 0;
        if (c->mTrapped.mWebbed) /* Web tile. */
            mCache[x][y].add (8, kRowBoom, spCre, recolor);
        /* Monster itself. */
        shGlyph g = c->mGlyph;
        mCache[x][y].add (g.mTileX, g.mTileY, spCre, recolor);
        if (frozen) /* Ice block tile. */
            mCache[x][y].add (7, kRowBoom, spCre);
        /* A small symbol tile in upper right corner. */
        if (c->isPet () and !c->isHero ()) /* Heart. */
            mCache[x][y].add (6, kRowTag2, spCre);
        else if (c->is (kAsleep)) /* Zzzzz. */
            mCache[x][y].add (1, kRowTag2, spCre);
    } else if (mem.mMon) {
        shGlyph g = MonIlks[mem.mMon].mGlyph;
        mCache[x][y].add (g.mTileX, g.mTileY, spCre);
    }

    /* Shield bubble. */
    if (s) {
        if (y < MAPMAXROWS-1) {
            shCreature *c = Level->getCreature (x, y+1);
            if (c and c->intr (kShielded) and c->countEnergy ())
                mCache[x][y].add (1, kRowShield, spCre);
        }
        if (x > 0) {
            shCreature *c = Level->getCreature (x-1, y);
            if (c and c->intr (kShielded) and c->countEnergy ())
                mCache[x][y].add (2, kRowShield, spCre);
        }
        if (y > 0) {
            shCreature *c = Level->getCreature (x, y-1);
            if (c and c->intr (kShielded) and c->countEnergy ())
                mCache[x][y].add (3, kRowShield, spCre);
        }
        if (x < MAPMAXCOLUMNS - 1) {
            shCreature *c = Level->getCreature (x+1, y);
            if (c and c->intr (kShielded) and c->countEnergy ())
                mCache[x][y].add (4, kRowShield, spCre);
        }

        shCreature *c = Level->getCreature (x, y);
        if (c and c->intr (kShielded) and c->countEnergy ())
            mCache[x][y].add (5, kRowShield, spCre);
    }
}

void
shXInterface::drawMem (int x, int y, shCreature *c,
                   shObjectIlk *o, shFeature *f, shSquare *s)
{
    eff::type e = Level->mEffects[x][y];
    shGlyph g = getMemASCII (x, y, e, c, o, f, s);
    mapGlyph (x, y, g);

    shMapLevel::shMemory mem = Level->getMemory (x, y);
    mCache[x][y].needs_update = true;
    mCache[x][y].reset ();

    if (s) {
        terr2tile (s->mTerr, &mCache[x][y], (x + y) % 2, 0);
    } else if (mem.mTerr) {
        terr2tile (mem.mTerr, &mCache[x][y], (x + y) % 2, 0);
    }
    if (f) {
        feat2tile (f, &mCache[x][y], 0);
    } else if (mem.mFeat != shFeature::kMaxFeatureType) {
        shFeature *real = Level->getFeature (x, y);
        shFeature fake;
        fake.mType = mem.mFeat;
        fake.mDoor.mFlags = mem.mDoor;
        if (real)
            fake.mTrapUnknown = real->mTrapUnknown;
        feat2tile (&fake, &mCache[x][y], 0);
    }
    if (o or mem.mObj) {
        shGlyph g = o ? o->mVague.mGlyph : AllIlks[mem.mObj].mVague.mGlyph;
        mCache[x][y].add (g.mTileX, g.mTileY, spObj);
    }
    if (c or mem.mMon) {
        shGlyph g = c ? c->mGlyph : MonIlks[mem.mMon].mGlyph;
        mCache[x][y].add (g.mTileX, g.mTileY, spCre);
    }
}

void
shXInterface::drawEffect (int x, int y, eff::type e)
{
    if (e == eff::none)
        return;

    drawSpecialEffect (e, &mCache[x][y]);
}
