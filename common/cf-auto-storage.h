/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#ifndef CF_AUTO_STORAGE_H
#define CF_AUTO_STORAGE_H

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <string.h>

#define CF_AUTO_STORAGE_PREFIX "auto-local:"

/* Keep directory components identical across C, Go and Python. Never accept
 * arbitrary paths through a library key or fall back from an invalid ID. */
static inline gboolean
cf_auto_key_valid (const char *key)
{
    size_t len = key ? strlen (key) : 0;
    size_t i;
    if (!len || len > 80)
        return FALSE;
    for (i = 0; i < len; ++i) {
        gboolean alnum = (key[i] >= 'a' && key[i] <= 'z') ||
                         (key[i] >= '0' && key[i] <= '9');
        if (!alnum && !(key[i] == '-' && i > 0 && i + 1 < len))
            return FALSE;
    }
    return TRUE;
}

static inline const char *
cf_auto_storage_key (const char *storage_id)
{
    const char *key;
    if (!storage_id || !g_str_has_prefix (storage_id, CF_AUTO_STORAGE_PREFIX))
        return NULL;
    key = storage_id + strlen (CF_AUTO_STORAGE_PREFIX);
    return cf_auto_key_valid (key) ? key : NULL;
}

/* The parent is a pre-mounted deployment root. Refuse symlink redirection at
 * the managed key boundary; only trusted maintenance owns this directory. */
static inline char *
cf_auto_storage_dir (const char *parent, const char *key)
{
    struct stat st;
    char *dir;
    if (!parent || !g_path_is_absolute (parent) || !cf_auto_key_valid (key) ||
        g_lstat (parent, &st) < 0 || !S_ISDIR (st.st_mode))
        return NULL;
    dir = g_build_filename (parent, key, NULL);
    if (g_mkdir (dir, 0700) < 0 && errno != EEXIST) {
        g_free (dir);
        return NULL;
    }
    if (g_lstat (dir, &st) < 0 || !S_ISDIR (st.st_mode)) {
        g_free (dir);
        return NULL;
    }
    return dir; /* Caller owns the allocated path. */
}
#endif
