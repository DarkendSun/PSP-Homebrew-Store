/* PSP Homebrew Store: browse a signed catalogue of free homebrew from the Internet Archive's
 * "PlayStation Portable Library: Homebrews", download it through the friends' relay and install it
 * into PSP/GAME/<folder>/. Third-party homebrew; use at your own risk. Never touches PSP/GAME/UPDATE. */
#include <pspkernel.h>
#include <pspctrl.h>
#include <psputils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "catalogue.h"
#include "install.h"
#include "net.h"
#include "tweetnacl.h"
#include "ui.h"
#include "util.h"
#include "version.h"

PSP_MODULE_INFO("hbstore", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-4096);   /* leave 4 MB for Sony's network modules and dialogs */

#ifndef HB_DEFAULT_RELAY
#define HB_DEFAULT_RELAY "http://psp-homebrew-relay.YOUR-SUBDOMAIN.workers.dev"
#endif
#define LIST_ROWS 27
#define MAX_CATALOGUE (4 * 1024 * 1024)

static volatile int running = 1;
static hb_catalogue cat;
static int *view, nview, sel, top, installed_only;
static long long free_bytes = -1;
static char search[64], relay[160] = HB_DEFAULT_RELAY, msg[512];

/* ------------------------------------------------------------ HOME button */

static int exit_cb(int a, int b, void *c)
{
	(void)a; (void)b; (void)c;
	running = 0;
	sceKernelExitGame();
	return 0;
}

static int cb_thread(SceSize args, void *argp)
{
	int id = sceKernelCreateCallback("exit", exit_cb, NULL);
	(void)args; (void)argp;
	sceKernelRegisterExitCallback(id);
	sceKernelSleepThreadCB();
	return 0;
}

/* ------------------------------------------------------------ settings + cache files */

static void app_path(const char *name, char *out, int outlen)
{
	snprintf(out, outlen, "%s/%s", hb_app_dir, name);
}

static void load_config(void)
{
	char path[HB_PATH], *line;
	unsigned char *buf = NULL;
	int len;
	app_path("config.txt", path, sizeof(path));
	if (hb_read_file(path, &buf, &len, 4096) < 0)
		return;
	for (line = strtok((char *)buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		if (!strncmp(line, "relay=", 6))
			snprintf(relay, sizeof(relay), "%s", line + 6);
	}
	free(buf);
	while (*relay && relay[strlen(relay) - 1] == '/')
		relay[strlen(relay) - 1] = 0;
}

static void save_config(void)
{
	char path[HB_PATH], body[300];
	app_path("config.txt", path, sizeof(path));
	snprintf(body, sizeof(body), "relay=%s\n", relay);
	hb_write_file(path, body, (int)strlen(body));
}

static void rebuild_view(void)
{
	int i;
	free(view);
	view = malloc(sizeof(int) * (cat.count + 1));
	nview = 0;
	for (i = 0; view && i < cat.count; i++) {
		hb_entry *e = &cat.entries[i];
		if (installed_only && !inst_installed(e))
			continue;
		if (*search && !hb_icase_contains(e->title, search) && !hb_icase_contains(e->kind, search) &&
		    !hb_icase_contains(e->desc, search))
			continue;
		view[nview++] = i;
	}
	if (sel >= nview) sel = nview ? nview - 1 : 0;
	if (top > sel) top = sel;
}

/* load catalogue text + signature (both already in memory); replaces the current one only if valid */
static int use_catalogue(const unsigned char *txt, int len, const char *sig, char *err, int errlen)
{
	hb_catalogue fresh;
	if (cat_load(&fresh, txt, len, sig, err, errlen) < 0)
		return -1;
	cat_free(&cat);
	cat = fresh;
	rebuild_view();
	return 0;
}

static void load_cached(void)
{
	char p1[HB_PATH], p2[HB_PATH], err[160];
	unsigned char *txt = NULL, *sig = NULL;
	int tl, sl;
	app_path("catalogue.txt", p1, sizeof(p1));
	app_path("catalogue.sig", p2, sizeof(p2));
	if (hb_read_file(p1, &txt, &tl, MAX_CATALOGUE) == 0 && hb_read_file(p2, &sig, &sl, 1024) == 0) {
		if (use_catalogue(txt, tl, (char *)sig, err, sizeof(err)) < 0)
			snprintf(msg, sizeof(msg), "Saved catalogue rejected: %s Press START to fetch it again.", err);
	} else {
		snprintf(msg, sizeof(msg), "No catalogue yet. Press START to connect and fetch it.");
	}
	free(txt);
	free(sig);
}

/* ------------------------------------------------------------ progress screen */

typedef struct { const char *what; u64 last; } prog_ctx;

static int progress(void *p, long long done, long long total)
{
	prog_ctx *c = p;
	u64 now = sceKernelGetSystemTimeWide();
	char a[16], b[16];
	if (ui_cancel_pressed())
		return 1;
	if (now - c->last < 100000 && done != total)
		return 0;
	c->last = now;
	hb_fmt_size(done, a, sizeof(a));
	hb_fmt_size(total, b, sizeof(b));
	ui_begin();
	ui_header(APP_NAME, NULL);
	ui_at(2, 6, C_TEXT, "%s", c->what);
	ui_bar(9, done, total);
	ui_at(2, 11, C_DIM, "%s of %s", a, total > 0 ? b : "?");
	ui_footer("O cancel");
	ui_end();
	return !running;
}

/* ------------------------------------------------------------ actions */

static int ensure_wifi(void)
{
	char err[200];
	if (net_ready())
		return 1;
	if (net_start(err, sizeof(err)) < 0) {
		ui_message("Wi-Fi", err);
		return 0;
	}
	return 1;
}

static void self_update(void)
{
	char ask[200], url[256], err[200], path[HB_PATH], eboot[HB_PATH], hex[129];
	unsigned char *buf = NULL, hash[64];
	int len = 0;
	prog_ctx pc = { "Downloading the new version of the store app...", 0 };

	snprintf(ask, sizeof(ask), "Version %s of the store app is out (you have %s). Download and install it now?",
	         cat.app_version, APP_VERSION);
	if (!ui_confirm("Update available", ask))
		return;
	snprintf(url, sizeof(url), "%s/app/EBOOT.PBP", relay);
	if (http_get(url, NULL, NULL, &buf, &len, 16 * 1024 * 1024, progress, &pc, err, sizeof(err)) < 0) {
		ui_message("Update", err);
		return;
	}
	crypto_hash(hash, buf, (unsigned long long)len);
	hb_hex(hash, 64, hex);
	if (len != cat.app_size || strcmp(hex, cat.app_sha512)) {
		free(buf);
		ui_message("Update", "The download doesn't match the signed catalogue, so it wasn't installed.");
		return;
	}
	app_path("EBOOT.new", path, sizeof(path));
	app_path("EBOOT.PBP", eboot, sizeof(eboot));
	if (hb_write_file(path, buf, len) < 0) {
		free(buf);
		ui_message("Update", "Couldn't write to the memory stick.");
		return;
	}
	free(buf);
	sceIoRemove(eboot);
	if (sceIoRename(path, eboot) < 0) {
		ui_message("Update", "Couldn't replace EBOOT.PBP. The new version is saved as EBOOT.new in the app's folder.");
		return;
	}
	ui_message("Updated", "Installed. The store will now close; start it again from Game > Memory Stick.");
	sceKernelExitGame();
}

static void refresh(void)
{
	char url[256], err[200];
	unsigned char *txt = NULL, *sig = NULL;
	int tl = 0, sl = 0;
	prog_ctx pc = { "Fetching the catalogue...", 0 };

	if (!ensure_wifi())
		return;
	snprintf(url, sizeof(url), "%s/catalogue.sig", relay);
	if (http_get(url, NULL, NULL, &sig, &sl, 1024, progress, &pc, err, sizeof(err)) < 0) {
		ui_message("Catalogue", err);
		return;
	}
	snprintf(url, sizeof(url), "%s/catalogue.txt", relay);
	if (http_get(url, NULL, NULL, &txt, &tl, MAX_CATALOGUE, progress, &pc, err, sizeof(err)) < 0) {
		free(sig);
		ui_message("Catalogue", err);
		return;
	}
	if (use_catalogue(txt, tl, (char *)sig, err, sizeof(err)) < 0) {
		ui_message("Catalogue", err);
	} else {
		char p[HB_PATH];
		app_path("catalogue.txt", p, sizeof(p));
		hb_write_file(p, txt, tl);
		app_path("catalogue.sig", p, sizeof(p));
		hb_write_file(p, sig, sl);
		msg[0] = 0;
		if (cat.app_size > 0 && hb_version_newer(cat.app_version, APP_VERSION))
			self_update();
	}
	free(txt);
	free(sig);
}

static void install(hb_entry *e)
{
	char ask[400], sz[16], fr[16], url[700], id[200], file[400], zip[HB_PATH], err[200], hex[41];
	unsigned char digest[20];
	long long free_now = hb_free_space();
	SceKernelUtilsSha1Context sha;
	prog_ctx pc = { "Downloading...", 0 };
	int r;

	hb_fmt_size(e->size, sz, sizeof(sz));
	hb_fmt_size(free_now, fr, sizeof(fr));
	if (free_now >= 0 && free_now < e->size * 3) {
		snprintf(ask, sizeof(ask), "%s needs about %s (download plus unpacked files) and only %s is free.",
		         e->title, sz, fr);
		ui_message("Not enough space", ask);
		return;
	}
	snprintf(ask, sizeof(ask), "Download %s (%s) and install it to PSP/GAME/%s%s?\n\n"
	         "This is third-party homebrew from the Internet Archive. You use it at your own risk.",
	         e->title, sz, e->apps[0].name, e->napps > 1 ? " and more" : "");
	if (!ui_confirm("Install", ask) || !ensure_wifi())
		return;

	inst_tmp_dir(zip, sizeof(zip));
	hb_mkdirs(zip);
	strcat(zip, "/download.zip");
	url_encode(e->id, id, sizeof(id));
	{
		/* encode each part of the file path separately, keeping the slashes */
		char part[400], *s, *slash;
		snprintf(part, sizeof(part), "%s", e->file);
		file[0] = 0;
		for (s = part; s; s = slash) {
			char enc[400];
			slash = strchr(s, '/');
			if (slash)
				*slash++ = 0;
			url_encode(s, enc, sizeof(enc));
			if (strlen(file) + strlen(enc) + 2 < sizeof(file)) {
				strcat(file, enc);
				if (slash)
					strcat(file, "/");
			}
		}
	}
	snprintf(url, sizeof(url), "%s/dl/%s/%s", relay, id, file);
	sceKernelUtilsSha1BlockInit(&sha);
	r = http_get(url, zip, &sha, NULL, NULL, 0, progress, &pc, err, sizeof(err));
	if (r < 0) {
		ui_message("Download", err);
		return;
	}
	sceKernelUtilsSha1BlockResult(&sha, digest);
	hb_hex(digest, 20, hex);
	if (strcasecmp(hex, e->sha1)) {
		sceIoRemove(zip);
		ui_message("Download", "The download doesn't match the catalogue's checksum, so it was deleted.");
		return;
	}
	pc.what = "Installing...";
	r = inst_install(e, zip, progress, &pc, err, sizeof(err));
	sceIoRemove(zip);
	if (r < 0) {
		ui_message("Install", err);
		return;
	}
	snprintf(ask, sizeof(ask), "%s is installed. Find it under Game > Memory Stick after you quit the store.",
	         e->title);
	ui_message("Installed", ask);
}

static void settings(void)
{
	for (;;) {
		unsigned b;
		char fr[16];
		hb_fmt_size(free_bytes, fr, sizeof(fr));
		ui_begin();
		ui_header("Settings", APP_VERSION);
		ui_at(1, 3, C_DIM, "Relay address:");
		ui_wrap(3, 4, UI_COLS - 4, 2, C_TEXT, relay);
		ui_at(1, 10, C_DIM, "Free space: %s", fr);
		ui_at(1, 11, C_DIM, "Catalogue: %d apps, made %s", cat.count, cat.generated ? cat.generated : "-");
		ui_wrap(1, 14, UI_COLS - 2, 8, C_DIM,
		        "Free homebrew from the Internet Archive's PlayStation Portable Library: Homebrews, "
		        "made by many different people. You use it at your own risk. The catalogue is signed "
		        "and every download is checked against it. Apps go to PSP/GAME/<folder>/; "
		        "PSP/GAME/UPDATE is never touched.");
		ui_footer("X edit relay   O back");
		ui_end();
		b = ui_pressed();
		if (b & PSP_CTRL_CROSS) {
			char v[160];
			if (ui_osk("Relay address (http://...)", relay, v, sizeof(v))) {
				snprintf(relay, sizeof(relay), "%s", v);
				save_config();
			}
		} else if ((b & PSP_CTRL_CIRCLE) || !running) {
			return;
		}
	}
}

static void details(hb_entry *e)
{
	int inst = inst_installed(e);
	for (;;) {
		unsigned b;
		char sz[16];
		int y, i;
		hb_fmt_size(e->size, sz, sizeof(sz));
		ui_begin();
		ui_header(e->title, inst ? "installed" : NULL);
		ui_at(1, 3, e->kind[0] == 'g' ? C_GAME : C_APP, "[%s]", e->kind);
		ui_at(20, 3, C_DIM, "%s   %s", e->date, sz);
		y = 5 + ui_wrap(1, 5, UI_COLS - 2, 14, C_TEXT, strcmp(e->desc, "-") ? e->desc : "(no description)");
		for (i = 0; i < e->napps; i++)
			ui_at(1, y + 1 + i, C_DIM, "Installs to PSP/GAME/%s", e->apps[i].name);
		ui_at(1, UI_ROWS - 6, C_DIM, "From: %s   by %s   license: %s", e->source, e->author,
		      strcmp(e->license, "-") ? e->license : "not stated");
		ui_wrap(1, UI_ROWS - 5, UI_COLS - 2, 2, C_DIM, e->link);
		ui_footer(inst ? "X reinstall   [] uninstall   O back" : "X download + install   O back");
		ui_end();
		b = ui_pressed();
		if (b & PSP_CTRL_CROSS) {
			install(e);
			inst = inst_installed(e);
			free_bytes = hb_free_space();
		} else if ((b & PSP_CTRL_SQUARE) && inst) {
			char err[160];
			if (ui_confirm("Uninstall", "Delete this app's folder(s) from PSP/GAME? Saved games in PSP/SAVEDATA stay."))
				if (inst_uninstall(e, err, sizeof(err)) < 0)
					ui_message("Uninstall", err);
			inst = inst_installed(e);
			free_bytes = hb_free_space();
		} else if ((b & PSP_CTRL_CIRCLE) || !running)
			return;
	}
}

static void list_screen(void)
{
	char right[48], fr[16];
	int i;
	unsigned b;
	hb_fmt_size(free_bytes, fr, sizeof(fr));
	snprintf(right, sizeof(right), "%d apps  free %s", nview, fr);
	ui_begin();
	ui_header(installed_only ? "Installed" : APP_NAME, right);
	if (*search)
		ui_at(0, 2, C_SEL, "Search: %s", search);
	if (!nview)
		ui_wrap(1, 4, UI_COLS - 2, 10, C_TEXT, *msg ? msg : "Nothing matches.");
	if (sel < top) top = sel;
	if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
	for (i = 0; i < LIST_ROWS && top + i < nview; i++) {
		hb_entry *e = &cat.entries[view[top + i]];
		int on = top + i == sel;
		/* icon slot: a coloured type tag (real ICON0 images are a later step) */
		ui_at(0, 3 + i, e->kind[0] == 'g' ? C_GAME : C_APP, "%c", e->kind[0] == 'g' ? 'G' : 'A');
		ui_at(2, 3 + i, on ? C_SEL : C_TEXT, "%s%-.52s", on ? "> " : "  ", e->title);
	}
	ui_footer("X open  /\\ search  [] installed  START fetch  SELECT settings");
	ui_end();

	b = ui_pressed();
	if (b & PSP_CTRL_DOWN && sel + 1 < nview) sel++;
	if (b & PSP_CTRL_UP && sel > 0) sel--;
	if (b & PSP_CTRL_RTRIGGER) sel = sel + LIST_ROWS < nview ? sel + LIST_ROWS : (nview ? nview - 1 : 0);
	if (b & PSP_CTRL_LTRIGGER) sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;
	if ((b & PSP_CTRL_CROSS) && nview)
		details(&cat.entries[view[sel]]);
	if (b & PSP_CTRL_TRIANGLE) {
		char v[64];
		if (ui_osk("Search", search, v, sizeof(v)))
			snprintf(search, sizeof(search), "%s", v);
		sel = top = 0;
		rebuild_view();
	}
	if (b & PSP_CTRL_SQUARE) {
		installed_only = !installed_only;
		sel = top = 0;
		rebuild_view();
	}
	if (b & PSP_CTRL_START)
		refresh();
	if (b & PSP_CTRL_SELECT)
		settings();
}

int main(int argc, char *argv[])
{
	int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
	if (th >= 0)
		sceKernelStartThread(th, 0, 0);
	hb_paths_init(argc > 0 ? argv[0] : NULL);
	ui_init();
	load_config();
	load_cached();
	free_bytes = hb_free_space();
	while (running)
		list_screen();
	sceKernelExitGame();
	return 0;
}
