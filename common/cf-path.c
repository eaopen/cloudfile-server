/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <string.h>

#include "cf-path.h"

char *
cf_path_normalize (const char *path)
{
    if (!path || *path == '\0')
        return g_strdup ("/");

    GString *buf = g_string_new ("");
    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        g_string_append_c (buf, '/');
        g_string_append_len (buf, start, p - start);
    }

    if (buf->len == 0)
        g_string_append_c (buf, '/');

    return g_string_free (buf, FALSE);
}

char *
cf_path_join (const char *dir, const char *entry)
{
    if (!entry || *entry == '\0')
        return cf_path_normalize (dir);

    /* Normalizing the concatenation rather than the two halves separately is
     * what makes ("/a", "/b//c") behave: the separator run in the middle is
     * just another run to collapse.
     */
    char *joined = g_strconcat (dir ? dir : "", "/", entry, NULL);
    char *norm = cf_path_normalize (joined);
    g_free (joined);

    return norm;
}

gboolean
cf_path_has_component (const char *path, const char *component)
{
    if (!path || !component || !*component)
        return FALSE;

    size_t len = strlen (component);
    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        if ((size_t)(p - start) == len &&
            strncmp (start, component, len) == 0)
            return TRUE;
    }

    return FALSE;
}
