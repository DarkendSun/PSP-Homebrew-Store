#ifndef HB_CATALOGUE_H
#define HB_CATALOGUE_H

#define HB_MAX_APPS 4

typedef struct {
	char *prefix;          /* folder inside the zip ("" = the zip's top level), ends with '/' */
	char *name;            /* folder under PSP/GAME */
} hb_app;

typedef struct {
	char *id, *title, *kind, *date, *file, *sha1, *desc;
	long long size;
	int napps;
	hb_app apps[HB_MAX_APPS];
} hb_entry;

typedef struct {
	char *text;            /* owns every string below */
	char *generated, *app_version, *app_sha512;
	long long app_size;
	hb_entry *entries;
	int count;
} hb_catalogue;

/* Verifies the ed25519 signature (sig is 128 hex chars) with the built-in public key, then parses.
 * Rejects an unsigned, altered or malformed catalogue. 0 on success. */
int cat_load(hb_catalogue *c, const unsigned char *txt, int len, const char *sighex, char *err, int errlen);
void cat_free(hb_catalogue *c);

#endif
