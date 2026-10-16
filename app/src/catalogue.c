#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "catalogue.h"
#include "tweetnacl.h"
#include "pubkey.h"
#include "util.h"

/* TweetNaCl wants this for key generation, which the app never does. */
void randombytes(unsigned char *x, unsigned long long n)
{
	(void)x;
	(void)n;
	abort();
}

static int verify(const unsigned char *txt, int len, const char *sighex)
{
	unsigned char *sm, *m;
	unsigned long long mlen;
	int ok;
	if (!sighex || strlen(sighex) < 128)
		return 0;
	sm = malloc(len + 64);
	m = malloc(len + 64);
	if (!sm || !m) {
		free(sm);
		free(m);
		return 0;
	}
	ok = hb_unhex(sighex, sm, 64) == 0;
	memcpy(sm + 64, txt, len);
	ok = ok && crypto_sign_open(m, &mlen, sm, (unsigned long long)len + 64, HB_PUBKEY) == 0 && mlen == (unsigned long long)len;
	free(sm);
	free(m);
	return ok;
}

static int split(char *line, char **f, int max)
{
	int n = 0;
	f[n++] = line;
	for (; *line && n < max; line++)
		if (*line == '\t') {
			*line = 0;
			f[n++] = line + 1;
		}
	return n;
}

static int parse_apps(hb_entry *e, char *s)
{
	e->napps = 0;
	while (s && *s && e->napps < HB_MAX_APPS) {
		char *next = strchr(s, '|'), *gt;
		if (next)
			*next++ = 0;
		gt = strchr(s, '>');
		if (!gt)
			return -1;
		*gt = 0;
		if ((*s && !hb_safe_relpath(s)) || !hb_safe_folder(gt + 1))
			return -1;
		e->apps[e->napps].prefix = s;
		e->apps[e->napps].name = gt + 1;
		e->napps++;
		s = next;
	}
	return e->napps > 0 ? 0 : -1;
}

int cat_load(hb_catalogue *c, const unsigned char *txt, int len, const char *sighex, char *err, int errlen)
{
	char *line, *next, *f[12];
	int lines = 0, n;

	memset(c, 0, sizeof(*c));
	if (!verify(txt, len, sighex)) {
		snprintf(err, errlen, "The catalogue's signature doesn't match, so it wasn't used.");
		return -1;
	}
	c->text = malloc(len + 1);
	if (!c->text) {
		snprintf(err, errlen, "Out of memory.");
		return -1;
	}
	memcpy(c->text, txt, len);
	c->text[len] = 0;
	for (n = 0; n < len; n++)
		if (c->text[n] == '\n')
			lines++;
	c->entries = calloc(lines + 1, sizeof(hb_entry));
	if (!c->entries) {
		cat_free(c);
		snprintf(err, errlen, "Out of memory.");
		return -1;
	}

	line = c->text;
	next = strchr(line, '\n');
	if (!next) goto bad;
	*next = 0;
	if (split(line, f, 12) < 6 || strcmp(f[0], "PSPHBS") || strcmp(f[1], "1"))
		goto bad;
	c->generated = f[2];
	c->app_version = f[3];
	c->app_size = strcmp(f[4], "-") ? atoll(f[4]) : 0;
	c->app_sha512 = f[5];

	for (line = next + 1; *line; line = next + 1) {
		hb_entry *e;
		next = strchr(line, '\n');
		if (!next) goto bad;
		*next = 0;
		if (split(line, f, 12) != 10 || strcmp(f[0], "E"))
			continue;   /* unknown line types are skipped, for newer catalogue versions */
		e = &c->entries[c->count];
		e->id = f[1];
		e->title = f[2];
		e->kind = f[3];
		e->date = f[4];
		e->file = f[5];
		e->size = atoll(f[6]);
		e->sha1 = f[7];
		e->desc = f[9];
		if (strlen(e->sha1) != 40 || parse_apps(e, f[8]) < 0)
			continue;
		c->count++;
	}
	return 0;

bad:
	cat_free(c);
	snprintf(err, errlen, "The catalogue is malformed.");
	return -1;
}

void cat_free(hb_catalogue *c)
{
	free(c->entries);
	free(c->text);
	memset(c, 0, sizeof(*c));
}
