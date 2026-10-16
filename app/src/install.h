#ifndef HB_INSTALL_H
#define HB_INSTALL_H

#include "catalogue.h"
#include "net.h"

/* 1 if every app folder of the entry is in PSP/GAME and was installed by this app */
int inst_installed(const hb_entry *e);
/* Unpack a verified zip into PSP/GAME/<folder>/ for each app (never PSP/GAME/UPDATE). */
int inst_install(const hb_entry *e, const char *zip_path, hb_progress cb, void *ctx, char *err, int errlen);
/* Remove the entry's folders, but only ones this app installed (marked with HBSTORE.ID). */
int inst_uninstall(const hb_entry *e, char *err, int errlen);
void inst_tmp_dir(char *out, int outlen);

#endif
