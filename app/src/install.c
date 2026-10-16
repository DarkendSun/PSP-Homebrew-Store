#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "install.h"
#include "util.h"
#include "zipx.h"

#define MARKER "HBSTORE.ID"

void inst_tmp_dir(char *out, int outlen)
{
	snprintf(out, outlen, "%s/PSP/HBSTORE_TMP", hb_dev);
}

static void game_dir(const char *name, char *out, int outlen)
{
	snprintf(out, outlen, "%s/PSP/GAME/%s", hb_dev, name);
}

/* 1 if the folder carries our marker for this entry id */
static int ours(const char *folder, const char *id)
{
	char path[HB_PATH];
	unsigned char *buf = NULL;
	int len = 0, same;
	snprintf(path, sizeof(path), "%s/%s", folder, MARKER);
	if (hb_read_file(path, &buf, &len, 1024) < 0)
		return 0;
	same = (int)strlen(id) == len && !memcmp(buf, id, len);
	free(buf);
	return same;
}

int inst_installed(const hb_entry *e)
{
	int i;
	char dir[HB_PATH];
	for (i = 0; i < e->napps; i++) {
		game_dir(e->apps[i].name, dir, sizeof(dir));
		if (!ours(dir, e->id))
			return 0;
	}
	return e->napps > 0;
}

int inst_install(const hb_entry *e, const char *zip_path, hb_progress cb, void *ctx, char *err, int errlen)
{
	char tmp[HB_PATH], stage[HB_PATH], target[HB_PATH], path[HB_PATH];
	int i, r;

	inst_tmp_dir(tmp, sizeof(tmp));
	for (i = 0; i < e->napps; i++) {
		const hb_app *a = &e->apps[i];
		if (!hb_safe_folder(a->name)) {   /* also refuses UPDATE and our own folder */
			snprintf(err, errlen, "Refusing the folder name '%s'.", a->name);
			return -1;
		}
		game_dir(a->name, target, sizeof(target));
		if (hb_exists(target) && !ours(target, e->id)) {
			snprintf(err, errlen, "PSP/GAME/%s already exists and wasn't installed by this app, so it was left alone.",
			         a->name);
			return -1;
		}
	}
	for (i = 0; i < e->napps; i++) {
		const hb_app *a = &e->apps[i];
		snprintf(stage, sizeof(stage), "%s/stage/%s", tmp, a->name);
		if (hb_exists(stage))
			hb_rmtree(stage);
		if (hb_mkdirs(stage) < 0) {
			snprintf(err, errlen, "Couldn't create a folder on the memory stick.");
			return -1;
		}
		r = zip_extract(zip_path, a->prefix, stage, cb, ctx, err, errlen);
		if (r < 0) {
			hb_rmtree(stage);
			return r;
		}
		snprintf(path, sizeof(path), "%s/EBOOT.PBP", stage);
		if (!hb_exists(path)) {
			hb_rmtree(stage);
			snprintf(err, errlen, "The zip had no EBOOT.PBP where the catalogue said.");
			return -1;
		}
		snprintf(path, sizeof(path), "%s/%s", stage, MARKER);
		hb_write_file(path, e->id, (int)strlen(e->id));

		game_dir(a->name, target, sizeof(target));
		if (hb_exists(target))
			hb_rmtree(target);   /* an older copy we installed ourselves */
		if (sceIoRename(stage, target) < 0) {
			/* the PSP can't move a folder to another directory, so copy it instead */
			if (hb_copytree(stage, target) < 0) {
				hb_rmtree(target);
				hb_rmtree(stage);
				snprintf(err, errlen, "Couldn't copy the app into PSP/GAME/%s (is the memory stick full?).", a->name);
				return -1;
			}
			hb_rmtree(stage);
		}
	}
	return 0;
}

int inst_uninstall(const hb_entry *e, char *err, int errlen)
{
	char dir[HB_PATH];
	int i, removed = 0;
	for (i = 0; i < e->napps; i++) {
		if (!hb_safe_folder(e->apps[i].name))
			continue;
		game_dir(e->apps[i].name, dir, sizeof(dir));
		if (!ours(dir, e->id))
			continue;   /* never delete folders we didn't install */
		if (hb_rmtree(dir) == 0)
			removed++;
	}
	if (!removed) {
		snprintf(err, errlen, "Nothing to remove: no folder installed by this app was found.");
		return -1;
	}
	return 0;
}
