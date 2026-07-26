/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <jansson.h>
#include <string.h>

#include "cf-fileop-json.h"
#include "log.h"
#include "seafile-error.h"

/* Returns an owned copy of a string member, or NULL when absent, null or the
 * empty string. Empty and absent are the same thing here: the Go side omits
 * nothing, so "" is how it spells "not applicable". */
static char *
dup_string_member (json_t *obj, const char *key)
{
    json_t *value = json_object_get (obj, key);
    if (!value || !json_is_string (value))
        return NULL;

    const char *str = json_string_value (value);
    if (!str || *str == '\0')
        return NULL;

    return g_strdup (str);
}

static GList *
dup_string_array (json_t *obj, const char *key)
{
    json_t *array = json_object_get (obj, key);
    if (!array || !json_is_array (array))
        return NULL;

    GList *list = NULL;
    size_t i;
    json_t *value;

    json_array_foreach (array, i, value) {
        if (!json_is_string (value))
            continue;
        list = g_list_append (list, g_strdup (json_string_value (value)));
    }

    return list;
}

CfFileOp *
cf_fileop_from_json (const char *json, GError **error)
{
    json_error_t jerror;
    json_t *obj = NULL;
    CfFileOp *fop = NULL;
    char *op = NULL;

    if (!json) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS,
                     "Empty file operation");
        return NULL;
    }

    obj = json_loadb (json, strlen (json), 0, &jerror);
    if (!obj || !json_is_object (obj)) {
        seaf_warning ("CloudFile: bad file operation payload: %s\n", jerror.text);
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS,
                     "Malformed file operation");
        if (obj)
            json_decref (obj);
        return NULL;
    }

    op = dup_string_member (obj, "op");

    /* Rejecting an unrecognised operation here rather than letting the
     * dispatcher see it keeps the failure at the edge, where the payload is
     * still available to log. Same reasoning as the dispatcher's own check:
     * an operation nobody recognises must not sail past every provider. */
    if (!cf_fileop_op_valid (op)) {
        seaf_warning ("CloudFile: unknown file operation '%s' from fileserver.\n",
                      op ? op : "(null)");
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS,
                     "Unknown file operation");
        g_free (op);
        json_decref (obj);
        return NULL;
    }

    fop = g_new0 (CfFileOp, 1);
    fop->op = op;
    fop->repo_id = dup_string_member (obj, "repo_id");
    fop->dir = dup_string_member (obj, "dir");
    fop->name = dup_string_member (obj, "name");
    fop->names = dup_string_array (obj, "names");
    fop->src_repo_id = dup_string_member (obj, "src_repo_id");
    fop->src_dir = dup_string_member (obj, "src_dir");
    fop->src_name = dup_string_member (obj, "src_name");
    fop->src_names = dup_string_array (obj, "src_names");
    fop->user = dup_string_member (obj, "user");
    fop->client = dup_string_member (obj, "client");
    fop->expect_commit_id = dup_string_member (obj, "expect_commit_id");
    fop->commit_id = dup_string_member (obj, "commit_id");
    fop->file_id = dup_string_member (obj, "file_id");

    json_decref (obj);

    return fop;
}

void
cf_fileop_json_free (CfFileOp *fop)
{
    if (!fop)
        return;

    /* The struct declares these const because providers must not rewrite
     * them; ownership is still ours for a parsed context. */
    g_free ((char *)fop->op);
    g_free ((char *)fop->repo_id);
    g_free ((char *)fop->dir);
    g_free ((char *)fop->name);
    g_list_free_full (fop->names, g_free);
    g_free ((char *)fop->src_repo_id);
    g_free ((char *)fop->src_dir);
    g_free ((char *)fop->src_name);
    g_list_free_full (fop->src_names, g_free);
    g_free ((char *)fop->user);
    g_free ((char *)fop->client);
    g_free ((char *)fop->expect_commit_id);
    g_free ((char *)fop->commit_id);
    g_free ((char *)fop->file_id);
    g_free (fop);
}
