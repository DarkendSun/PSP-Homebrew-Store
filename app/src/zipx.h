#ifndef HB_ZIPX_H
#define HB_ZIPX_H

#include "net.h"

/* Extract every file under `prefix` ("" = all) into dest_dir, stripping the prefix.
 * Stored and deflated entries only; refuses encrypted/zip64 entries and unsafe paths.
 * Returns the number of files written, -2 if cancelled, -1 on error. */
int zip_extract(const char *zip_path, const char *prefix, const char *dest_dir,
                hb_progress cb, void *ctx, char *err, int errlen);

#endif
