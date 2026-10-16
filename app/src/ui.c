/* Text UI on the debug screen (double-buffered), plus Sony's own dialogs (Wi-Fi, keyboard),
 * which need the GU running while they're shown. */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspgu.h>
#include <psputility.h>
#include <pspnet_apctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"

#define VRAM ((unsigned char *)0x44000000)   /* uncached VRAM */
#define FRAME 0x88000                        /* 512 * 272 * 4 */

static int back;                             /* 0 or 1: which buffer we draw into */
static unsigned last_buttons;
static int repeat_wait;
static unsigned __attribute__((aligned(16))) gu_list[0x10000];

void ui_init(void)
{
	pspDebugScreenInitEx(VRAM, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
	back = 1;
}

void ui_begin(void)
{
	pspDebugScreenSetBase((u32 *)(VRAM + back * FRAME));
	pspDebugScreenSetBackColor(0x00201008);
	pspDebugScreenEnableBackColor(1);
	pspDebugScreenClear();
}

void ui_end(void)
{
	sceDisplayWaitVblankStart();
	sceDisplaySetFrameBuf(VRAM + back * FRAME, 512, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
	back ^= 1;
}

void ui_at(int x, int y, unsigned color, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (x + (int)strlen(buf) > UI_COLS)
		buf[UI_COLS - x > 0 ? UI_COLS - x : 0] = 0;
	pspDebugScreenSetXY(x, y);
	pspDebugScreenSetTextColor(color);
	pspDebugScreenPuts(buf);
}

void ui_header(const char *title, const char *right)
{
	ui_at(0, 0, C_TITLE, "%s", title);
	if (right)
		ui_at(UI_COLS - (int)strlen(right), 0, C_DIM, "%s", right);
	ui_at(0, 1, C_DIM, "------------------------------------------------------------");
}

void ui_footer(const char *keys)
{
	ui_at(0, UI_ROWS - 2, C_DIM, "------------------------------------------------------------");
	ui_at(0, UI_ROWS - 1, C_TEXT, "%s", keys);
}

/* word-wrap text; returns lines used */
int ui_wrap(int x, int y, int width, int maxlines, unsigned color, const char *text)
{
	int lines = 0;
	while (*text && lines < maxlines) {
		int n = (int)strlen(text), cut = n;
		char line[UI_COLS + 1];
		if (n > width) {
			cut = width;
			while (cut > 0 && text[cut] != ' ')
				cut--;
			if (cut == 0)
				cut = width;
		}
		memcpy(line, text, cut);
		line[cut] = 0;
		ui_at(x, y + lines, color, "%s", line);
		lines++;
		text += cut;
		while (*text == ' ')
			text++;
	}
	return lines;
}

void ui_bar(int y, long long done, long long total)
{
	char bar[52];
	int i, fill = total > 0 ? (int)(done * 50 / total) : 0;
	if (fill > 50) fill = 50;
	for (i = 0; i < 50; i++)
		bar[i] = i < fill ? '#' : '.';
	bar[50] = 0;
	ui_at(2, y, C_GOOD, "[%s] %3d%%", bar, total > 0 ? (int)(done * 100 / total) : 0);
}

unsigned ui_pressed(void)
{
	SceCtrlData pad;
	unsigned now, fresh;
	sceCtrlReadBufferPositive(&pad, 1);
	now = pad.Buttons;
	fresh = now & ~last_buttons;
	if (now & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
		if (fresh & (PSP_CTRL_UP | PSP_CTRL_DOWN))
			repeat_wait = 20;
		else if (--repeat_wait <= 0) {
			fresh |= now & (PSP_CTRL_UP | PSP_CTRL_DOWN);
			repeat_wait = 4;
		}
	}
	last_buttons = now;
	return fresh;
}

int ui_cancel_pressed(void)
{
	SceCtrlData pad;
	int hit;
	sceCtrlPeekBufferPositive(&pad, 1);
	hit = (pad.Buttons & PSP_CTRL_CIRCLE) && !(last_buttons & PSP_CTRL_CIRCLE);
	last_buttons = pad.Buttons;
	return hit;
}

static int modal(const char *title, const char *text, const char *keys, int ask)
{
	for (;;) {
		unsigned b;
		ui_begin();
		ui_header(title, NULL);
		ui_wrap(1, 3, UI_COLS - 2, UI_ROWS - 7, C_TEXT, text);
		ui_footer(keys);
		ui_end();
		b = ui_pressed();
		if (b & PSP_CTRL_CROSS)
			return 1;
		if (b & PSP_CTRL_CIRCLE)
			return ask ? 0 : 1;
	}
}

int ui_confirm(const char *title, const char *text)
{
	return modal(title, text, "X yes    O no", 1);
}

void ui_message(const char *title, const char *text)
{
	modal(title, text, "X / O  OK", 0);
}

/* ---- Sony utility dialogs: run the GU while they are on screen */

static void gu_start(void)
{
	sceGuInit();
	sceGuStart(GU_DIRECT, gu_list);
	sceGuDrawBuffer(GU_PSM_8888, (void *)0, 512);
	sceGuDispBuffer(480, 272, (void *)FRAME, 512);
	sceGuDepthBuffer((void *)(2 * FRAME), 512);
	sceGuOffset(2048 - 240, 2048 - 136);
	sceGuViewport(2048, 2048, 480, 272);
	sceGuDepthRange(0xc350, 0x2710);
	sceGuScissor(0, 0, 480, 272);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuFinish();
	sceGuSync(0, 0);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
}

static void gu_clear(void)
{
	sceGuStart(GU_DIRECT, gu_list);
	sceGuClearColor(0xff201008);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(0, 0);
}

static void gu_stop(void)
{
	sceGuTerm();
	ui_init();
}

static void dialog_common(pspUtilityDialogCommon *base, unsigned size)
{
	base->size = size;
	sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &base->language);
	sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &base->buttonSwap);
	base->graphicsThread = 17;
	base->accessThread = 19;
	base->fontThread = 18;
	base->soundThread = 16;
}

int ui_netconf(void)
{
	pspUtilityNetconfData data;
	struct pspUtilityNetconfAdhoc adhoc;
	int state = 0;

	memset(&data, 0, sizeof(data));
	memset(&adhoc, 0, sizeof(adhoc));
	dialog_common(&data.base, sizeof(data));
	data.action = PSP_NETCONF_ACTION_CONNECTAP;
	data.adhocparam = &adhoc;

	gu_start();
	if (sceUtilityNetconfInitStart(&data) >= 0) {
		for (;;) {
			int st;
			gu_clear();
			st = sceUtilityNetconfGetStatus();
			if (st == PSP_UTILITY_DIALOG_VISIBLE)
				sceUtilityNetconfUpdate(1);
			else if (st == PSP_UTILITY_DIALOG_QUIT)
				sceUtilityNetconfShutdownStart();
			else if (st == PSP_UTILITY_DIALOG_NONE)
				break;
			sceDisplayWaitVblankStart();
			sceGuSwapBuffers();
		}
	}
	gu_stop();
	sceNetApctlGetState(&state);
	return state == PSP_NET_APCTL_STATE_GOT_IP;
}

int ui_osk(const char *prompt, const char *initial, char *out, int outlen)
{
	unsigned short desc[64], in[128], text[128];
	SceUtilityOskData data;
	SceUtilityOskParams params;
	int i, changed = 0;

	for (i = 0; i < 63 && prompt[i]; i++) desc[i] = (unsigned char)prompt[i];
	desc[i] = 0;
	for (i = 0; i < 127 && initial[i]; i++) in[i] = (unsigned char)initial[i];
	in[i] = 0;
	memset(text, 0, sizeof(text));

	memset(&data, 0, sizeof(data));
	data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
	data.lines = 1;
	data.unk_24 = 1;
	data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
	data.desc = desc;
	data.intext = in;
	data.outtextlength = 127;
	data.outtextlimit = outlen - 1 < 127 ? outlen - 1 : 127;
	data.outtext = text;

	memset(&params, 0, sizeof(params));
	dialog_common(&params.base, sizeof(params));
	params.datacount = 1;
	params.data = &data;

	gu_start();
	if (sceUtilityOskInitStart(&params) >= 0) {
		for (;;) {
			int st;
			gu_clear();
			st = sceUtilityOskGetStatus();
			if (st == PSP_UTILITY_DIALOG_VISIBLE)
				sceUtilityOskUpdate(1);
			else if (st == PSP_UTILITY_DIALOG_QUIT)
				sceUtilityOskShutdownStart();
			else if (st == PSP_UTILITY_DIALOG_NONE)
				break;
			sceDisplayWaitVblankStart();
			sceGuSwapBuffers();
		}
	}
	gu_stop();
	if (data.result == PSP_UTILITY_OSK_RESULT_CHANGED) {
		for (i = 0; i < outlen - 1 && text[i]; i++)
			out[i] = text[i] < 128 ? (char)text[i] : '?';
		out[i] = 0;
		changed = 1;
	}
	return changed;
}
