/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <string.h>

#include <jansson.h>

#include "log.h"
#include "seafile-session.h"
#include "cf-ext.h"
#include "cf-v2-acl.h"
#include "cf-v2-share.h"
#include "cloudfile-policy.h"

/* One path, one check: the request is a fixed shape, so the bound only has to
 * catch a path that already escaped the RPC's own validation. */
#define CF_SHARE_PAYLOAD_MAX 8192
#define CF_SHARE_PATH_MAX 4096
#define CF_SHARE_USER_MAX 255

/*
 * Strip the one spelling difference the Hub will not accept.
 *
 * callers hand this seam whatever the upstream RPC received, and several of
 * them carry a directory path with a trailing slash. The V2 canonical form has
 * none, and the endpoint answers 400 for it -- a valid share root would then
 * read as "no qualification" for some callers and work for others, which is
 * exactly the kind of per-entry-point difference this seam exists to remove.
 * Everything else (dot segments, empty segments, backslashes, control bytes) is
 * left for the Hub to reject, so there is still one canonicalizer.
 */
static char *
request_path (const char *path)
{
    if (!path || path[0] != '/' || strlen (path) > CF_SHARE_PATH_MAX)
        return NULL;

    char *normalized = g_strdup (path);
    size_t length = strlen (normalized);
    while (length > 1 && normalized[length - 1] == '/')
        normalized[--length] = '\0';

    /* The library root is never a share root: a share is strictly below it, and
     * asking would only spend a round trip to be told "invisible". */
    if (length == 1) {
        g_free (normalized);
        return NULL;
    }
    return normalized;
}

static char *
qualified_permission (const char *repo_id, const char *path, const char *user,
                      gboolean *unavailable)
{
    json_t *request = json_object ();
    json_t *checks = json_array ();
    json_t *check = json_pack ("{s:s,s:s,s:s}", "path", path, "kind", "dir",
                               "action", "read");
    json_t *username = json_string (user);
    json_t *repo = json_string (repo_id);
    json_t *reply = NULL, *permissions = NULL;
    GString *body = g_string_new (NULL);
    char *payload = NULL;
    char *result = NULL;

    /* Non-stealing setters throughout, so every local has exactly one owner and
     * the single cleanup below is the whole story. */
    if (!request || !checks || !check || !username || !repo ||
        json_array_append (checks, check) ||
        json_object_set (request, "checks", checks) ||
        json_object_set (request, "username", username) ||
        json_object_set (request, "repo_id", repo))
        goto done;

    payload = json_dumps (request, JSON_COMPACT);
    if (!payload || strlen (payload) > CF_SHARE_PAYLOAD_MAX)
        goto done;

    long status = cf_v2_acl_check_json (seaf, payload, body);
    if (status != 200) {
        /* 5xx and transport failures are outages, not answers, and must not be
         * reported to the user as "no permission" (the Hub turns this into 503
         * with a distinguishable code). 4xx is the authority saying no: 403 for
         * a path outside every qualified root, 400 for a path it rejects, 404
         * for an account with no business subject. */
        if (status < 0 || status >= 500) {
            if (unavailable) *unavailable = TRUE;
        }
        goto done;
    }

    reply = json_loadb (body->str, body->len, JSON_REJECT_DUPLICATES, NULL);
    if (!json_is_true (json_object_get (reply, "allowed")) ||
        g_strcmp0 (json_string_value (json_object_get (reply, "contract")),
                   "cf-acl-v1"))
        goto done;

    /* The level, not just the verdict: an r-qualified root must not become
     * uploadable. A Hub without this field is older than this seam, and the
     * only safe reading is then "cannot qualify" -- never "assume rw". */
    permissions = json_object_get (reply, "permissions");
    if (!json_is_array (permissions) || json_array_size (permissions) != 1)
        goto done;
    const char *level = json_string_value (json_array_get (permissions, 0));
    if (!g_strcmp0 (level, "rw") || !g_strcmp0 (level, "r"))
        result = g_strdup (level);

done:
    json_decref (request);
    json_decref (checks);
    json_decref (check);
    json_decref (username);
    json_decref (repo);
    json_decref (reply);
    g_free (payload);
    g_string_free (body, TRUE);
    return result;
}

char *
cf_v2_share_grant (const char *repo_id, const char *path, const char *user,
                   gboolean *unavailable)
{
    if (unavailable) *unavailable = FALSE;
    if (!seaf || !repo_id || strlen (repo_id) != 36 ||
        !user || !*user || strlen (user) > CF_SHARE_USER_MAX)
        return NULL;

    char *normalized = request_path (path);
    if (!normalized)
        return NULL;

    char *permission = qualified_permission (repo_id, normalized, user, unavailable);
    g_free (normalized);
    return permission;
}

void
cf_v2_share_init (void)
{
    if (!seaf)
        return;

    /*
     * Only where the V2 authority exists.
     *
     * The qualification is answered by the Hub, so on a deployment without the
     * managed schema the endpoint does not exist and every native denial would
     * pay a network attempt to learn that. cf_acl_init() gates the tightening
     * capability on the same fact, which keeps "V2 is installed" a single
     * question with a single answer.
     */
    if (!cf_policy_managed_guard_required (seaf->db, seaf->config))
        return;

    /* Default on: this closes a denial hole. A deployment that never edits
     * seafile.conf would otherwise keep the bug while believing it shipped the
     * fix. An operator who wants the old behaviour can still say so. */
    if (!cf_ext_config_bool_default ("dir_share_native_grant", TRUE))
        return;

    if (!seaf->seahub_url || !*seaf->seahub_url ||
        !seaf->seahub_pk || !*seaf->seahub_pk) {
        /* Without the internal channel a share-only account can only be
         * refused. Say so once at startup instead of failing silently per
         * request, where it would look like a missing share. */
        seaf_warning ("CloudFile: no internal Hub channel; directory shares "
                      "cannot qualify native writes.\n");
        return;
    }

    cf_ext_register_share_grant ("v2-dir-share", cf_v2_share_grant);
}
