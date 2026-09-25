/* RVIP additions (~/Games/RVIP.md), ported from ~/Games/zapm/Rvip.cpp:
 * auto-explore (X), '<'/'>' walk to the nearest known stairs, command menu
 * on Enter, inventory list with a cursor and item menus. Hooked in via
 * shInterface::rvipCommand () in shCreature::playerControl (Hero.cpp) and
 * shCreature::listInventory (). */
#include <string.h>
#include <stdlib.h>
#include "Global.h"
#include "Util.h"
#include "Map.h"
#include "Interface.h"
#include "Hero.h"
#include "Object.h"
#ifdef PRIME_X11
#include "../port/XUI.h"
#else
static bool x11KeyPending () { return false; }
#endif

int RvipMsgs;                   /* bumped by shInterface::vp () */
static int mode;                /* 0 off, 1 explore, 2 to '>', 3 to '<' */
static int msgs0, reopen;
static int listByLetter;        /* rvipList: chosen by its letter (not Enter) */
static shMapLevel *lev;
static unsigned char seen[MAPMAXCOLUMNS][MAPMAXROWS], visited[MAPMAXCOLUMNS][MAPMAXROWS],
    tried[MAPMAXCOLUMNS][MAPMAXROWS];

void rvipStop () { mode = 0; }

void
shInterface::pushKey (int k)
{
#ifdef PRIME_X11
    x11PushKey (k);
#else
    (void) k;
#endif
}

/* The key bound to a command, printable keys first; -1 if none. */
int
shInterface::keyFor (Command c)
{
    for (int k = ' ' + 1; k < 127; k++) if (keyToCommand (k) == c) return k;
    for (int k = 1; k < 0x130; k++) if (keyToCommand (k) == c) return k;
    return -1;
}

/* A floating list with a cursor in the kMenu window, sized to its content.
   key[i] == NULL: header row. Returns the index chosen, -1 on Escape / 0.
   A key that isn't a list key is returned in *other (and -1). */
int
rvipList (const char *title, int n, const char **text, const char **key,
          int *cur, int *other = NULL)
{
    int i, w = strlen (title), kw = 1, top = 0, rows = mini (n, 21), res = -1;
    for (i = 0; i < n; i++) if (key[i]) kw = maxi (kw, strlen (key[i]));
    for (i = 0; i < n; i++) w = maxi (w, (int) strlen (text[i]) + (key[i] ? kw + 3 : 0));
    w = mini (w + 1, 79);
    if (other) *other = 0;
    listByLetter = 0;
    while (*cur < n - 1 && !key[*cur]) ++*cur;
    I->newWin (shInterface::kMenu);
    while (1) {
        char buf[128];
        if (*cur < top) top = *cur;
        if (*cur >= top + rows) top = *cur - rows + 1;
        if (top > 0 && !key[top - 1] && top - 1 >= *cur - rows + 1) top--; /* keep header */
        I->clearWin (shInterface::kMenu);
        I->setWinColor (shInterface::kMenu, kWhite, kBlack);
        I->winPrint (shInterface::kMenu, 0, 0, "%s", title);
        for (i = top; i < n && i < top + rows; i++) {
            if (!key[i]) {
                snprintf (buf, sizeof buf, "%-*s", w, text[i]);
                I->setWinColor (shInterface::kMenu, kYellow, kBlack);
            } else {
                snprintf (buf, sizeof buf, " %*s  %-*s", kw, key[i], w - kw - 3, text[i]);
                I->setWinColor (shInterface::kMenu, i == *cur ? kBlack : kGray,
                                i == *cur ? kWhite : kBlack);
            }
            buf[w] = 0;
            I->winPrint (shInterface::kMenu, 1 + i - top, 0, "%s", buf);
        }
        I->setWinColor (shInterface::kMenu, kGray, kBlack);
        if (n > rows)
            I->winPrint (shInterface::kMenu, rows + 1, 0, "%s",
                         top + rows < n ? "--More--" : "--End--");
        shInterface::SpecialKey sk;
        int k = I->getSpecialChar (&sk);
        int step = 0;
        if (KEY_CLICK == k) { /* click = cursor + Enter */
            i = top + UIClickRow - 1;
            if (UIClickWin != shInterface::kMenu || i < top || i >= n || i >= top + rows || !key[i])
                continue;
            *cur = i;
            res = i;
            break;
        }
        if ('8' == k || shInterface::kUpArrow == sk) step = -1;
        else if ('2' == k || shInterface::kDownArrow == sk) step = 1;
        else if ('5' == k || shInterface::kEnter == sk || ' ' == k) { res = *cur; break; }
        else if (27 == k || '0' == k || '.' == k) break;
        else {
            for (i = 0; i < n; i++)
                if (key[i] && !key[i][1] && key[i][0] == k) break;
            if (i < n) { res = *cur = i; listByLetter = 1; break; }
            if (other) { *other = k; break; }
        }
        if (step) {
            i = *cur;
            do i += step; while (i >= 0 && i < n && !key[i]);
            if (i >= 0 && i < n) *cur = i;
        }
    }
    I->delWin (shInterface::kMenu);
    return res;
}

static int
hostileInView ()
{
    shCreature *h = Hero.cr ();
    for (int i = 0; i < Level->mCrList.count (); ++i) {
        shCreature *c = Level->mCrList.get (i);
        /* sessile ones (fungi, turrets that stay put) only when close */
        if (c != h && c->isHostile () && h->canSee (c) &&
            (!c->feat (kSessile) || maxi (abs (c->mX - h->mX), abs (c->mY - h->mY)) <= 2))
            return 1;
    }
    return 0;
}

static int
passable (int x, int y)
{
    shFeature *f = Level->getKnownFeature (x, y);
    shCreature *h = Hero.cr ();
    if (!seen[x][y]) return 0;
    if (f && f->isTrap () && !f->mTrapUnknown) return 0;
    if (f && f->isDoor () && f->isClosedDoor ()) return !tried[x][y];
    if (Level->isObstacle (x, y) || Level->isWatery (x, y)) return 0;
    shCreature *c = Level->getCreature (x, y);
    return !c || c == h || !h->canSee (c);
}

static int
target (int x, int y)
{
    if (2 == mode || 3 == mode) {
        shFeature *f = Level->getKnownFeature (x, y);
        return f && f->mType == (2 == mode ? shFeature::kStairsDown : shFeature::kStairsUp);
    }
    if (visited[x][y]) return 0;
    if (Level->countObjects (x, y)) return 1;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (Level->isInBounds (x + dx, y + dy) && !seen[x + dx][y + dy]) return 1;
    return 0;
}

/* Next step of the current walk, or kNoCommand when done/stopped. */
shInterface::Command
shInterface::rvipStep ()
{
    static short px[MAPMAXCOLUMNS][MAPMAXROWS], py[MAPMAXCOLUMNS][MAPMAXROWS];
    static int qx[MAPMAXCOLUMNS * MAPMAXROWS], qy[MAPMAXCOLUMNS * MAPMAXROWS];
    shCreature *hero = Hero.cr ();
    int x, y, h = 0, t = 0, found = 0, hx = hero->mX, hy = hero->mY;

    if (RvipMsgs != msgs0 || hostileInView () || x11KeyPending ()) return kNoCommand;
    visited[hx][hy] = 1;
    if (target (hx, hy) && mode > 1)
        return 2 == mode ? kMoveDown : kMoveUp;
    memset (px, -1, sizeof px);
    px[hx][hy] = hx; py[hx][hy] = hy;
    qx[t] = hx; qy[t++] = hy;
    while (h < t) {
        x = qx[h]; y = qy[h++];
        if ((x != hx || y != hy) && target (x, y)) { found = 1; break; }
        shFeature *door = Level->getKnownFeature (x, y);
        for (int d = 0; d < 8; d++) {
            int nx = x, ny = y;
            if (!Level->moveForward ((shDirection) d, &nx, &ny) || px[nx][ny] >= 0 ||
                !passable (nx, ny))
                continue;
            /* no diagonal steps through doorways */
            shFeature *nd = Level->getKnownFeature (nx, ny);
            if ((d & 1) && ((door && door->isDoor ()) || (nd && nd->isDoor ()))) continue;
            px[nx][ny] = x; py[nx][ny] = y;
            qx[t] = nx; qy[t++] = ny;
        }
    }
    if (!found) {
        p (1 == mode ? "Nothing left to explore (secret doors? search by resting)."
                     : "You don't know a way to those stairs.");
        return kNoCommand;
    }
    while (px[x][y] != hx || py[x][y] != hy) {
        int ox = x; x = px[ox][y]; y = py[ox][y];
    }
    shFeature *f = Level->getFeature (x, y);
    if (f && f->isDoor () && f->isClosedDoor ())
        tried[x][y] = 1;  /* bumping opens it; if locked it's skipped next time */
    return (Command) (kMoveN + vectorDirection (x - hx, y - hy));
}

/* The actions that fit an item; the first is the main action. */
static int
itemActions (shObject *o, shInterface::Command *c, const char **l)
{
    int n = 0;
#define ADD(_c, _l) (c[n] = shInterface::_c, l[n++] = _l)
    if (o->isA (kCanister)) ADD (kQuaff, "Quaff");
    if (o->isA (kFloppyDisk)) ADD (kExecute, "Execute");
    if (o->isA (kArmor) || o->isA (kImplant)) {
        if (o->is (obj::worn)) ADD (kTakeOff, "Take off");
        else ADD (kWear, o->isA (kImplant) ? "Install" : "Wear");
    }
    if (o->isA (kRayGun)) ADD (kZapRayGun, "Zap");
    if (o->isUseable ()) ADD (kUse, "Use / apply");
    if ((o->isA (kWeapon) || o->isA (kRayGun)) && !o->is (obj::wielded)) ADD (kWield, "Wield");
    if (o->isThrownWeapon () || o->isA (kCanister)) ADD (kThrow, "Throw");
    ADD (kExamine, "Examine");
    if (o->is (obj::unpaid)) ADD (kPay, "Pay");
    ADD (kDrop, "Drop");
    ADD (kName, "Name");
    ADD (kAdjust, "Adjust letter");
#undef ADD
    return n;
}

/* 'i': list with a cursor. Letter = main action, Enter/Space/5 = action
   menu, numpad + main action, - drop, * examine, 0 / . / Esc close; any
   other key is a normal command. Returns the time taken. */
int
rvipInventory ()
{
    static int cur;
    shCreature *h = Hero.cr ();
    while (1) {
        int n = h->mInventory->count (), i;
        if (!n) { I->p ("You aren't carrying anything!"); return 0; }
        const char *text[64], *keys[64];
        static char lines[64][100], ks[64][2];
        h->reorganizeInventory ();
        n = mini (n, 64);
        for (i = 0; i < n; i++) {
            shObject *o = h->mInventory->get (i);
            snprintf (lines[i], 100, "%s", o->inv ());
            text[i] = lines[i];
            ks[i][0] = o->mLetter; ks[i][1] = 0;
            keys[i] = ks[i];
        }
        if (cur >= n) cur = n - 1;
        int k, act;               /* 0 main, 1 menu, 2 drop, 3 examine */
        int pick = rvipList ("Inventory   (Enter: actions  -: drop  *: examine)", n, text, keys, &cur, &k);
        act = listByLetter ? 0 : 1;   /* letter: main action; Enter/5/space: menu */
        if (pick < 0) {
            if ('+' == k) { pick = cur; act = 0; }
            else if ('-' == k) { pick = cur; act = 2; }
            else if ('*' == k) { pick = cur; act = 3; }
            else if (k >= 1 && k <= 26) { /* Ctrl+letter examines */
                for (i = 0; i < n; i++) if (ks[i][0] == 'a' + k - 1) { pick = cur = i; act = 3; }
            }
            if (pick < 0) {
                if (k) I->pushKey (k);  /* any other key: a command */
                return 0;
            }
        }
        shObject *o = h->mInventory->get (pick);
        shInterface::Command c[16];
        const char *l[16];
        int na = itemActions (o, c, l);
        if (1 == act) {
            const char *ak[16];
            static char akb[16][12];
            char title[100];
            for (i = 0; i < na; i++) {
                int key = I->keyFor (c[i]);
                snprintf (akb[i], 12, "%s", key < 0 ? "" : key < ' ' ? "^" : "");
                if (key > 0 && key < ' ') akb[i][1] = key + '@', akb[i][2] = 0;
                else if (key > 0 && key < 127) akb[i][0] = key, akb[i][1] = 0;
                else if (key > 0) snprintf (akb[i], 12, "%s", I->getKeyForCommand (c[i]));
                ak[i] = akb[i];
            }
            snprintf (title, sizeof title, "%c - %s", o->mLetter, o->inv ());
            int ac = 0;
            i = rvipList (title, na, l, ak, &ac);
            if (i < 0) continue;
            act = 9 + i;
        }
        shInterface::Command cmd = 0 == act ? c[0] : 2 == act ? shInterface::kDrop
            : 3 == act ? shInterface::kExamine : c[act - 9];
        I->pushKey (I->keyFor (cmd));
        int elapsed = h->objectVerbCommand (o);
        if (shInterface::kExamine == cmd || shInterface::kName == cmd
            || shInterface::kAdjust == cmd) {
            if (!elapsed) continue;   /* free actions: back to the list */
        }
        reopen = 1;
        return elapsed;
    }
}

/* Enter: every command, grouped like the help. */
struct MenuEntry { shInterface::Command cmd; const char *text; };
static const MenuEntry menuEntries[] = {
    { shInterface::kNoCommand, "Movement" },
    { shInterface::kExplore, "Explore automatically (any key stops)" },
    { shInterface::kMoveDown, "Go down (walks to known stairs)" },
    { shInterface::kMoveUp, "Go up (walks to known stairs)" },
    { shInterface::kGlide, "Glide: move until something happens" },
    { shInterface::kRest, "Rest (and search)" },
    { shInterface::kNoCommand, "Fighting" },
    { shInterface::kFireWeapon, "Fire wielded weapon" },
    { shInterface::kThrow, "Throw an object" },
    { shInterface::kZapRayGun, "Zap a ray gun" },
    { shInterface::kKick, "Kick something" },
    { shInterface::kMutantPower, "Use a mutant power" },
    { shInterface::kNoCommand, "Inventory" },
    { shInterface::kListInventory, "Browse your inventory" },
    { shInterface::kPickup, "Pick up items from the floor" },
    { shInterface::kPickUpAll, "Pick up whole stack of items" },
    { shInterface::kDrop, "Drop an item" },
    { shInterface::kDropMany, "Drop several items" },
    { shInterface::kExamine, "Examine object closely" },
    { shInterface::kWield, "Hold something in hands (wield)" },
    { shInterface::kWear, "Wear armor, install implants" },
    { shInterface::kTakeOff, "Remove armor, weapons, implants" },
    { shInterface::kUse, "Apply or activate (use)" },
    { shInterface::kQuaff, "Drink object contents (quaff)" },
    { shInterface::kExecute, "Execute (run object's program)" },
    { shInterface::kName, "Name an object or its class" },
    { shInterface::kAdjust, "Adjust shortcut letter" },
    { shInterface::kToggleAutopickup, "Toggle autopickup option" },
    { shInterface::kSwap, "Swap wielded and prepared item" },
    { shInterface::kNoCommand, "Environment" },
    { shInterface::kOpen, "Open a door, operate droids" },
    { shInterface::kClose, "Close a door" },
    { shInterface::kLook, "Look at a feature or monster" },
    { shInterface::kPay, "Pay for an item or service" },
    { shInterface::kNoCommand, "Character" },
    { shInterface::kCharScreen, "Character screen" },
    { shInterface::kEditSkills, "Open skill screen" },
    { shInterface::kHistory, "Show log message history" },
    { shInterface::kDiscoveries, "Show recognized item types" },
    { shInterface::kNoCommand, "Game" },
    { shInterface::kHelp, "Help on keys and game topics" },
    { shInterface::kMainMenu, "Game menu: options, save, quit" },
    { shInterface::kBOFHPower, "Debug command (BOFH mode)" },
};

shInterface::Command
shInterface::rvipMenu ()
{
    static int cur;
    const int N = sizeof menuEntries / sizeof menuEntries[0];
    const char *text[N], *keys[N];
    static char kb[N][16];
    shInterface::Command cmds[N];
    int n = 0;
    for (int i = 0; i < N; ++i) {
        shInterface::Command c = menuEntries[i].cmd;
        text[n] = menuEntries[i].text;
        cmds[n] = c;
        if (c == kNoCommand) { keys[n++] = NULL; continue; }
        if (c == kBOFHPower && !BOFH) continue;
        const char *k = getKeyForCommand (c);
        int key = keyFor (c);
        if (key > ' ' && key < 127) snprintf (kb[n], 16, "%c", key);
        else snprintf (kb[n], 16, "%s", k ? k : "");
        if (!kb[n][0]) continue; /* not bound in this keymap */
        keys[n] = kb[n];
        n++;
    }
    int i = rvipList ("Commands", n, text, keys, &cur);
    return i < 0 ? kNoCommand : cmds[i];
}

/* Replaces getCommand () in shCreature::playerControl (). */
shInterface::Command
shInterface::rvipCommand ()
{
    shCreature *hero = Hero.cr ();
    if (lev != Level) {
        lev = Level; mode = 0;
        memset (seen, 0, sizeof seen); memset (visited, 0, sizeof visited);
        memset (tried, 0, sizeof tried);
    }
    for (int x = 0; x < MAPMAXCOLUMNS; x++)
        for (int y = 0; y < MAPMAXROWS; y++)
            if (hero->canSee (x, y) || Level->getMemory (x, y).mTerr != kMaxTerrainType)
                seen[x][y] = 1;
    if (mode) {
        Command c = rvipStep ();
        if (kNoCommand != c) { msgs0 = RvipMsgs; return c; }
        mode = 0;
    }
    if (reopen) {
        reopen = 0;
        if (!hostileInView ()) return kListInventory;
    }
    while (1) {
#ifdef __EMSCRIPTEN__
        extern int RvipAtPrompt;  /* the page may autosave now */
        RvipAtPrompt = 1;
        Command c = getCommand ();
        RvipAtPrompt = 0;
#else
        Command c = getCommand ();
#endif
        int onstairs = 0;
        shFeature *f = Level->getFeature (hero->mX, hero->mY);
        if (kCmdMenu == c) {
            c = rvipMenu ();
            if (kNoCommand == c) continue;
        }
        if (kMoveDown == c)
            onstairs = hero->isTrapped () || (f && (shFeature::kStairsDown == f->mType ||
                shFeature::kHole == f->mType || shFeature::kTrapDoor == f->mType ||
                shFeature::kPit == f->mType || shFeature::kAcidPit == f->mType ||
                shFeature::kSewagePit == f->mType));
        if (kMoveUp == c) onstairs = (f && shFeature::kStairsUp == f->mType) || hero->isTrapped ();
        if (kExplore == c || ((kMoveDown == c || kMoveUp == c) && !onstairs)) {
            mode = kExplore == c ? 1 : kMoveDown == c ? 2 : 3;
            if (hostileInView ()) { p ("Not with a monster in view."); mode = 0; continue; }
            msgs0 = RvipMsgs;
            c = rvipStep ();
            if (kNoCommand == c) { mode = 0; continue; }
            return c;
        }
        return c;
    }
}
