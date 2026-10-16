#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "zipx.h"
#include "util.h"

static unsigned char inbuf[16384], outbuf[32768];

static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static int fail(char *err, int errlen, const char *msg)
{
	snprintf(err, errlen, "%s", msg);
	return -1;
}

/* copy or inflate one entry's data from f (positioned at the data) into out; checks size and CRC */
static int unpack(FILE *f, FILE *out, unsigned method, unsigned csize, unsigned usize, unsigned crc_want,
                  long long *done, long long total, hb_progress cb, void *ctx)
{
	unsigned long crc = crc32(0L, Z_NULL, 0);
	unsigned remaining = csize, written = 0;
	if (method == 0) {
		while (remaining) {
			unsigned n = remaining < sizeof(outbuf) ? remaining : sizeof(outbuf);
			if (fread(outbuf, 1, n, f) != n || fwrite(outbuf, 1, n, out) != n)
				return -1;
			crc = crc32(crc, outbuf, n);
			remaining -= n;
			written += n;
			*done += n;
			if (cb && cb(ctx, *done, total))
				return -2;
		}
	} else {
		z_stream zs;
		int ret = Z_OK;
		memset(&zs, 0, sizeof(zs));
		if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
			return -1;
		while (ret != Z_STREAM_END) {
			unsigned w;
			if (zs.avail_in == 0) {
				unsigned n = remaining < sizeof(inbuf) ? remaining : sizeof(inbuf);
				if (n == 0 || fread(inbuf, 1, n, f) != n) {
					inflateEnd(&zs);
					return -1;
				}
				remaining -= n;
				*done += n;
				zs.next_in = inbuf;
				zs.avail_in = n;
				if (cb && cb(ctx, *done, total)) {
					inflateEnd(&zs);
					return -2;
				}
			}
			zs.next_out = outbuf;
			zs.avail_out = sizeof(outbuf);
			ret = inflate(&zs, Z_NO_FLUSH);
			if (ret != Z_OK && ret != Z_STREAM_END) {
				inflateEnd(&zs);
				return -1;
			}
			w = sizeof(outbuf) - zs.avail_out;
			if (written + w > usize || fwrite(outbuf, 1, w, out) != w) {
				inflateEnd(&zs);
				return -1;
			}
			crc = crc32(crc, outbuf, w);
			written += w;
		}
		inflateEnd(&zs);
	}
	return (written == usize && crc == crc_want) ? 0 : -1;
}

int zip_extract(const char *zip_path, const char *prefix, const char *dest_dir,
                hb_progress cb, void *ctx, char *err, int errlen)
{
	FILE *f = fopen(zip_path, "rb");
	unsigned char *tail = NULL, *cd = NULL, *eocd = NULL;
	long size, tlen, i;
	unsigned entries, cdsize, cdoff, pos, k;
	int files = 0, rc = -1, plen = (int)strlen(prefix);
	long long done = 0;

	if (!f)
		return fail(err, errlen, "Couldn't open the download.");
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	tlen = size < 65557 ? size : 65557;
	tail = malloc(tlen);
	if (!tail || fseek(f, size - tlen, SEEK_SET) || fread(tail, 1, tlen, f) != (size_t)tlen) {
		fail(err, errlen, "Couldn't read the zip.");
		goto out;
	}
	for (i = tlen - 22; i >= 0; i--)
		if (u32(tail + i) == 0x06054b50) {
			eocd = tail + i;
			break;
		}
	if (!eocd) {
		fail(err, errlen, "That isn't a zip file.");
		goto out;
	}
	entries = u16(eocd + 10);
	cdsize = u32(eocd + 12);
	cdoff = u32(eocd + 16);
	if (cdoff == 0xffffffff || (long)cdoff + (long)cdsize > size) {
		fail(err, errlen, "Unsupported zip (zip64 or damaged).");
		goto out;
	}
	cd = malloc(cdsize);
	if (!cd || fseek(f, cdoff, SEEK_SET) || fread(cd, 1, cdsize, f) != cdsize) {
		fail(err, errlen, "Couldn't read the zip's directory.");
		goto out;
	}

	for (pos = 0, k = 0; k < entries; k++) {
		char name[HB_PATH], outpath[HB_PATH], *slash;
		unsigned flags, method, crc, csize, usize, nlen, elen, clen, lho;
		unsigned char lh[30];
		FILE *out;
		int r;

		if (pos + 46 > cdsize || u32(cd + pos) != 0x02014b50) {
			fail(err, errlen, "The zip's directory is damaged.");
			goto out;
		}
		flags = u16(cd + pos + 8);
		method = u16(cd + pos + 10);
		crc = u32(cd + pos + 16);
		csize = u32(cd + pos + 20);
		usize = u32(cd + pos + 24);
		nlen = u16(cd + pos + 28);
		elen = u16(cd + pos + 30);
		clen = u16(cd + pos + 32);
		lho = u32(cd + pos + 42);
		if (pos + 46 + nlen > cdsize || nlen >= sizeof(name)) {
			fail(err, errlen, "The zip's directory is damaged.");
			goto out;
		}
		memcpy(name, cd + pos + 46, nlen);
		name[nlen] = 0;
		pos += 46 + nlen + elen + clen;
		for (slash = name; *slash; slash++)
			if (*slash == '\\')
				*slash = '/';

		if (nlen == 0 || name[nlen - 1] == '/')
			continue;                       /* directory entry: folders are made as needed */
		if (plen && strncmp(name, prefix, plen))
			continue;                       /* not part of this app */
		if (!hb_safe_relpath(name + plen)) {
			fail(err, errlen, "The zip contains an unsafe path, so nothing was installed.");
			goto out;
		}
		if (flags & 1) {
			fail(err, errlen, "The zip is password-protected.");
			goto out;
		}
		if (method != 0 && method != 8) {
			fail(err, errlen, "The zip uses a compression method the PSP app can't unpack.");
			goto out;
		}
		if (csize == 0xffffffff || usize == 0xffffffff) {
			fail(err, errlen, "Unsupported zip (zip64).");
			goto out;
		}
		if (snprintf(outpath, sizeof(outpath), "%s/%s", dest_dir, name + plen) >= (int)sizeof(outpath)) {
			fail(err, errlen, "A path in the zip is too long.");
			goto out;
		}
		slash = strrchr(outpath, '/');
		*slash = 0;
		if (hb_mkdirs(outpath) < 0) {
			fail(err, errlen, "Couldn't create a folder on the memory stick.");
			goto out;
		}
		*slash = '/';

		if (fseek(f, lho, SEEK_SET) || fread(lh, 1, 30, f) != 30 || u32(lh) != 0x04034b50 ||
		    fseek(f, lho + 30 + u16(lh + 26) + u16(lh + 28), SEEK_SET)) {
			fail(err, errlen, "The zip is damaged.");
			goto out;
		}
		out = fopen(outpath, "wb");
		if (!out) {
			fail(err, errlen, "Couldn't write to the memory stick.");
			goto out;
		}
		r = unpack(f, out, method, csize, usize, crc, &done, size, cb, ctx);
		if (fclose(out) != 0 && r == 0)
			r = -1;
		if (r == -2) {
			snprintf(err, errlen, "Cancelled.");
			rc = -2;
			goto out;
		}
		if (r < 0) {
			fail(err, errlen, "A file in the zip didn't unpack correctly (bad data or the stick is full).");
			goto out;
		}
		files++;
	}
	if (!files) {
		fail(err, errlen, "The zip didn't contain the app's files.");
		goto out;
	}
	rc = files;

out:
	free(tail);
	free(cd);
	fclose(f);
	return rc;
}
