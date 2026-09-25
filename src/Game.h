#ifndef GAME_H
#define GAME_H

int nameOK (const char *name);

int newGame (const char *name);

int loadGame (const char *name);

int saveGame (const char *path = NULL); /* RVIP: path for the web autosave */

void gameLoop ();

/* exitPRIME(const code)  declared in Global.h */

struct HighScoreEntry
{
    int mScore;
    int mUid;
    char mName[30];
    char mMessage[200];
};
#define NUMHIGHSCORES 100

#endif
