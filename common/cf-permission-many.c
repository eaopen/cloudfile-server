/* Batch RPC transport; deliberately no authorization cache or shared decision. */
#include "cf-permission-many.h"
#include <jansson.h>
#include <string.h>

#define MAX_PATHS 50
#define MAX_BYTES 65536
#define MAX_PATH_BYTES 4096
#define BUDGET_US (10 * G_USEC_PER_SEC)

static gboolean
valid_repo(const char *repo)
{
    if (!repo || strlen(repo) != 36) return FALSE;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (repo[i] != '-') return FALSE;
        } else if (!g_ascii_isxdigit(repo[i])) return FALSE;
    }
    return TRUE;
}

/* Match decoded JSON path rules at the Hub: no URL decoding or dot folding.
 * Normalize one trailing slash while preserving root and Unicode spelling. */
static char *
checked_path(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) > MAX_PATH_BYTES || strstr(path, "//"))
        return NULL;
    char **parts = g_strsplit(path, "/", -1);
    gboolean valid = g_strv_length(parts) <= 130;
    for (char **part = parts; *part; ++part)
        if (!strcmp(*part, ".") || !strcmp(*part, "..")) valid = FALSE;
    g_strfreev(parts);
    if (!valid) return NULL;
    char *result = g_strdup(path);
    size_t n = strlen(result);
    if (n > 1 && result[n - 1] == '/') result[n - 1] = 0;
    return result;
}

char *
cf_permissions_many_json(const char *request_json, CfPermissionScalar scalar, GError **error)
{
    json_t *request = NULL, *response = NULL, *items = NULL;
    char *result = NULL;
    GPtrArray *normalized = g_ptr_array_new_with_free_func(g_free);
    GError *failure = NULL;
    gint64 deadline = g_get_monotonic_time() + BUDGET_US;
    if (!request_json || strlen(request_json) > MAX_BYTES || !scalar) goto invalid;
    /* Reject duplicate keys, invalid UTF-8 and embedded NUL instead of silently
     * changing the identity/path seen by the scalar C string interface. */
    request = json_loads(request_json, JSON_REJECT_DUPLICATES, NULL);
    const char *repo = json_string_value(json_object_get(request, "repo_id"));
    const char *user = json_string_value(json_object_get(request, "user"));
    json_t *paths = json_object_get(request, "paths");
    json_t *version = json_object_get(request, "version");
    if (!json_is_object(request) || json_object_size(request) != 4 ||
        !json_is_integer(version) || json_integer_value(version) != 1 ||
        !valid_repo(repo) || !user || !*user || strlen(user) > 255 ||
        !json_is_array(paths) || json_array_size(paths) < 1 ||
        json_array_size(paths) > MAX_PATHS) goto invalid;
    /* Validate the entire request before invoking any provider. Duplicates keep
     * their original slots and are evaluated independently, never cached. */
    for (size_t i = 0; i < json_array_size(paths); ++i) {
        char *path = checked_path(json_string_value(json_array_get(paths, i)));
        if (!path) goto invalid;
        g_ptr_array_add(normalized, path);
    }
    items = json_array();
    for (guint i = 0; i < normalized->len; ++i) {
        const char *path = g_ptr_array_index(normalized, i);
        if (g_get_monotonic_time() >= deadline) goto timeout;
        char *permission = scalar(repo, path, user, &failure);
        if (failure) { g_free(permission); goto out; }
        if (g_get_monotonic_time() >= deadline) { g_free(permission); goto timeout; }
        /* This is a read-search transport: unknown scalar permission strings
         * remain a denial, exactly as in the old Hub scalar adapter. */
        gboolean readable = permission && (!strcmp(permission, "r") || !strcmp(permission, "rw"));
        json_t *item = json_pack("{s:s,s:o}", "path", path, "permission",
                                readable ? json_string(permission) : json_null());
        g_free(permission);
        if (!item || json_array_append_new(items, item) < 0) goto invalid;
    }
    response = json_pack("{s:i,s:s,s:s,s:O}", "version", 1, "repo_id", repo,
                         "user", user, "items", items);
    result = json_dumps(response, JSON_COMPACT);
    if (!result) goto invalid;
    goto out;
invalid:
    g_set_error_literal(&failure, g_quark_from_static_string("cf-permission-many"), 1,
                        "Invalid permission batch");
    goto out;
timeout:
    g_set_error_literal(&failure, g_quark_from_static_string("cf-permission-many"), 2,
                        "Permission batch time budget exceeded");
out:
    /* Provider errors discard all accumulated results. Legacy providers which
     * expose only NULL still mean deny; no synthetic allow is ever returned. */
    if (failure) { g_clear_pointer(&result, g_free); g_propagate_error(error, failure); }
    json_decref(response);
    json_decref(items);
    json_decref(request);
    g_ptr_array_free(normalized, TRUE);
    return result;
}
