/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#ifndef CF_DIR_PAGE_H
#define CF_DIR_PAGE_H

#include <glib.h>

/* Raw scan state survives invalid-object skips and capability filtering.
 * It deliberately contains no permission decisions or persistent state. */
typedef struct {
    int scanned_count;
    gboolean scan_exhausted;
} CfDirScan;

/* Newly allocated JSON string; caller g_free(). The contained listing is
 * authorized afresh, while continuation describes the unfiltered window. */
char *cf_list_dir_page_json (const char *request_json, GError **error);

#endif
