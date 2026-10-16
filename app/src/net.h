#ifndef HB_NET_H
#define HB_NET_H

#include <psputils.h>

/* return nonzero from the callback to cancel */
typedef int (*hb_progress)(void *ctx, long long done, long long total);

int net_start(char *err, int errlen);        /* load Sony's net modules, connect with the Wi-Fi dialog */
int net_ready(void);

/* Plain-HTTP GET (the PSP can't do modern TLS; the relay does HTTPS to archive.org).
 * To memory: *mem gets a malloc'd buffer (NUL-terminated) of at most maxmem bytes.
 * To a file: out_path, with an optional running SHA-1. Returns 0, -2 if cancelled, -1 on error. */
int http_get(const char *url, const char *token, const char *out_path, SceKernelUtilsSha1Context *sha1,
             unsigned char **mem, int *memlen, int maxmem, hb_progress cb, void *ctx, char *err, int errlen);

void url_encode(const char *in, char *out, int outlen);

#endif
