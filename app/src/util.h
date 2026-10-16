#ifndef HB_UTIL_H
#define HB_UTIL_H

#define HB_PATH 512

/* "ms0:" or "ef0:" (PSP Go internal storage), taken from where the EBOOT was started */
extern char hb_dev[8];
extern char hb_app_dir[HB_PATH];   /* e.g. ms0:/PSP/GAME/HBSTORE */

void hb_paths_init(const char *argv0);
void hb_hex(const unsigned char *in, int n, char *out);
int hb_unhex(const char *in, unsigned char *out, int n);
int hb_exists(const char *path);
int hb_is_dir(const char *path);
int hb_mkdirs(const char *dir);
int hb_rmtree(const char *path);
int hb_copytree(const char *src, const char *dst);
long long hb_free_space(void);
int hb_read_file(const char *path, unsigned char **buf, int *len, int maxlen);
int hb_write_file(const char *path, const void *buf, int len);
int hb_version_newer(const char *a, const char *b);
int hb_safe_folder(const char *name);
int hb_safe_relpath(const char *path);
void hb_fmt_size(long long n, char *out, int outlen);
int hb_icase_contains(const char *hay, const char *needle);

#endif
