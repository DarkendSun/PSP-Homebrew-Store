#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "util.h"

char hb_dev[8] = "ms0:";
char hb_app_dir[HB_PATH] = "ms0:/PSP/GAME/HBSTORE";

void hb_paths_init(const char *argv0)
{
	const char *colon, *slash;
	if (!argv0)
		return;
	colon = strchr(argv0, ':');
	slash = strrchr(argv0, '/');
	if (colon && colon - argv0 < (int)sizeof(hb_dev) - 1) {
		memcpy(hb_dev, argv0, colon - argv0 + 1);
		hb_dev[colon - argv0 + 1] = 0;
	}
	if (slash && slash - argv0 < HB_PATH - 1) {
		memcpy(hb_app_dir, argv0, slash - argv0);
		hb_app_dir[slash - argv0] = 0;
	}
}

void hb_hex(const unsigned char *in, int n, char *out)
{
	static const char d[] = "0123456789abcdef";
	int i;
	for (i = 0; i < n; i++) {
		out[2 * i] = d[in[i] >> 4];
		out[2 * i + 1] = d[in[i] & 15];
	}
	out[2 * n] = 0;
}

static int nib(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int hb_unhex(const char *in, unsigned char *out, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		int hi = nib(in[2 * i]), lo = hi < 0 ? -1 : nib(in[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (unsigned char)(hi << 4 | lo);
	}
	return 0;
}

int hb_exists(const char *path)
{
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

int hb_is_dir(const char *path)
{
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0 && FIO_S_ISDIR(st.st_mode);
}

int hb_mkdirs(const char *dir)
{
	char tmp[HB_PATH];
	char *p;
	if (strlen(dir) >= sizeof(tmp))
		return -1;
	strcpy(tmp, dir);
	p = strchr(tmp, ':');
	p = p ? p + 2 : tmp + 1;   /* skip "ms0:/" */
	for (; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (!hb_is_dir(tmp))
				sceIoMkdir(tmp, 0777);
			*p = '/';
		}
	}
	if (!hb_is_dir(tmp))
		sceIoMkdir(tmp, 0777);
	return hb_is_dir(tmp) ? 0 : -1;
}

int hb_rmtree(const char *path)
{
	SceIoDirent ent;
	SceUID d;
	char child[HB_PATH];
	if (!hb_is_dir(path))
		return sceIoRemove(path) < 0 ? -1 : 0;
	d = sceIoDopen(path);
	if (d < 0)
		return -1;
	for (;;) {
		memset(&ent, 0, sizeof(ent));
		if (sceIoDread(d, &ent) <= 0)
			break;
		if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, ".."))
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, ent.d_name) >= (int)sizeof(child))
			continue;
		if (FIO_S_ISDIR(ent.d_stat.st_mode))
			hb_rmtree(child);
		else
			sceIoRemove(child);
	}
	sceIoDclose(d);
	return sceIoRmdir(path) < 0 ? -1 : 0;
}

typedef struct {
	unsigned long maxClusters, freeClusters;
	int unk1;
	unsigned int sectorSize, sectorCount;
} hb_devinfo;

long long hb_free_space(void)
{
	hb_devinfo info;
	hb_devinfo *cmd = &info;
	memset(&info, 0, sizeof(info));
	if (sceIoDevctl(hb_dev, 0x02425818, &cmd, sizeof(cmd), NULL, 0) < 0)
		return -1;
	return (long long)info.freeClusters * info.sectorCount * info.sectorSize;
}

int hb_read_file(const char *path, unsigned char **buf, int *len, int maxlen)
{
	SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
	int size, got;
	if (f < 0)
		return -1;
	size = (int)sceIoLseek(f, 0, PSP_SEEK_END);
	sceIoLseek(f, 0, PSP_SEEK_SET);
	if (size < 0 || size > maxlen) {
		sceIoClose(f);
		return -1;
	}
	*buf = malloc(size + 1);
	if (!*buf) {
		sceIoClose(f);
		return -1;
	}
	got = sceIoRead(f, *buf, size);
	sceIoClose(f);
	if (got != size) {
		free(*buf);
		*buf = NULL;
		return -1;
	}
	(*buf)[size] = 0;
	*len = size;
	return 0;
}

int hb_write_file(const char *path, const void *buf, int len)
{
	SceUID f = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	int put;
	if (f < 0)
		return -1;
	put = sceIoWrite(f, buf, len);
	sceIoClose(f);
	return put == len ? 0 : -1;
}

/* 1 if dotted version a is newer than b */
int hb_version_newer(const char *a, const char *b)
{
	while (*a || *b) {
		long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
		if (x != y)
			return x > y;
		if (*a == '.') a++;
		if (*b == '.') b++;
		if (!isdigit((unsigned char)*a) && *a) break;
		if (!isdigit((unsigned char)*b) && *b) break;
	}
	return 0;
}

/* A folder name we may create or delete under PSP/GAME: never UPDATE (the updater) or our own folder. */
int hb_safe_folder(const char *name)
{
	const char *p;
	int n = (int)strlen(name);
	if (n == 0 || n > 40 || !strcmp(name, ".") || !strcmp(name, ".."))
		return 0;
	if (!strcasecmp(name, "UPDATE") || !strcasecmp(name, "HBSTORE"))
		return 0;
	for (p = name; *p; p++)
		if (*p < 32 || *p > 126 || strchr("/\\:*?\"<>|", *p))
			return 0;
	return name[0] != ' ' && name[n - 1] != ' ' && name[n - 1] != '.';
}

/* A relative path inside an app folder: no leading slash, no "..", no drive letters */
int hb_safe_relpath(const char *path)
{
	const char *seg = path;
	if (!*path || *path == '/' || strchr(path, ':') || strchr(path, '\\'))
		return 0;
	for (;;) {
		const char *end = strchr(seg, '/');
		int len = end ? (int)(end - seg) : (int)strlen(seg);
		if (len == 2 && seg[0] == '.' && seg[1] == '.')
			return 0;
		if (!end)
			return 1;
		seg = end + 1;
	}
}

void hb_fmt_size(long long n, char *out, int outlen)
{
	if (n < 0)
		snprintf(out, outlen, "?");
	else if (n < 1024 * 1024)
		snprintf(out, outlen, "%d KB", (int)((n + 1023) / 1024));
	else
		snprintf(out, outlen, "%d.%d MB", (int)(n / (1024 * 1024)), (int)((n % (1024 * 1024)) * 10 / (1024 * 1024)));
}

int hb_icase_contains(const char *hay, const char *needle)
{
	int n = (int)strlen(needle);
	if (!n)
		return 1;
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return 0;
}
