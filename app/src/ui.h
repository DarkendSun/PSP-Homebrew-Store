#ifndef HB_UI_H
#define HB_UI_H

#include <pspctrl.h>

#define UI_COLS 60
#define UI_ROWS 34

/* colours are 0x00BBGGRR */
#define C_TEXT   0x00e0e0e0
#define C_DIM    0x00909090
#define C_TITLE  0x0040d0ff
#define C_SEL    0x0000ffff
#define C_GOOD   0x0060ff60
#define C_BAD    0x006060ff
#define C_GAME   0x00ff9040
#define C_APP    0x0040c0ff

void ui_init(void);
void ui_begin(void);            /* start drawing a frame into the back buffer */
void ui_end(void);              /* show it (waits for vblank) */
void ui_at(int x, int y, unsigned color, const char *fmt, ...);
void ui_header(const char *title, const char *right);
void ui_footer(const char *keys);
int ui_wrap(int x, int y, int width, int maxlines, unsigned color, const char *text);
void ui_bar(int y, long long done, long long total);

unsigned ui_pressed(void);      /* buttons newly pressed this frame (UP/DOWN repeat while held) */
int ui_cancel_pressed(void);    /* non-blocking: was CIRCLE pressed since the last call? */

int ui_confirm(const char *title, const char *text);   /* X = yes, O = no */
void ui_message(const char *title, const char *text);  /* X or O closes */
int ui_osk(const char *prompt, const char *initial, char *out, int outlen);
int ui_netconf(void);           /* Sony's own Wi-Fi dialog; 1 when connected */

#endif
