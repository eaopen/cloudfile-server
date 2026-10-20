/* Real scalar/provider/boundary code; only native storage I/O is fake. */
#include <glib.h>
#include <jansson.h>
#include <string.h>
#include "cf-permission-many.h"
#define SEAFILE_DOMAIN (g_quark_from_static_string("fixture"))
#define SEAF_ERR_BAD_ARGS 503
#define SEAF_ERR_GENERAL 500
static struct { void *repo_mgr; } session;
static __typeof__(session) *seaf = &session;
static gboolean cf_acl_on = TRUE;
static int counters[4]; /* qualification, managed-boundary check, hook, share */
static int fail_at, timeout_at, deny_repo, managed, boundary_unavailable;
static gint64 now;
gint64 fixture_clock(void) { return now; }
static gboolean is_uuid_valid(const char *repo) { return g_uuid_string_is_valid(repo); }
static char *seaf_repo_manager_check_permission(void *mgr, const char *repo, const char *user, GError **error)
{
    ++counters[0];
    if (counters[0] == fail_at) {
        g_set_error_literal(error, SEAFILE_DOMAIN, 500, "injected provider failure");
        return NULL;
    }
    if (counters[0] == timeout_at) now += 10 * G_USEC_PER_SEC;
    return deny_repo ? NULL : g_strdup("rw");
}
/* 2026-10-15 v0.9-only: the retired v0.8 rule loader is replaced by the
 * managed-library boundary; the fixture flips it per test case.
 * boundary_unavailable models an unreadable runtime (schema drift / SQL
 * failure): it must refuse AND report the reason, never look like a denial. */
gboolean cf_acl_legacy_entry_denied(const char *repo, gboolean *unavailable)
{
    ++counters[1];
    if (managed) return TRUE;
    if (boundary_unavailable) {
        if (unavailable) *unavailable = TRUE;
        return TRUE;
    }
    return FALSE;
}
static char *provider_hook(const char *repo, const char *path, const char *user,
                           const char *native, gboolean *unavailable)
{
    (void)unavailable;
    ++counters[2];
    return strcmp(path, "/hook") ? g_strdup(native) : NULL;
}
static char *cf_acl_apply(const char *, const char *, const char *, const char *, gboolean *);
typedef struct { char *(*perm)(const char *, const char *, const char *, const char *, gboolean *); } CfProvider;
static CfProvider acl = {cf_acl_apply}, hook = {provider_hook};
static GList *providers;

/* Independent qualification (cf-ext.h). The real dispatcher runs over one stub
 * capability, so the tests exercise the seam the server calls instead of a
 * reimplementation of it. counters[3] counts capability invocations: "was the
 * share question even asked" is the assertion that matters most. */
static char *share_level;
static int share_unavailable;
static char *share_capability(const char *repo, const char *path, const char *user,
                              gboolean *unavailable)
{
    (void)repo; (void)path; (void)user;
    ++counters[3];
    if (share_unavailable) { if (unavailable) *unavailable = TRUE; return NULL; }
    return share_level ? g_strdup(share_level) : NULL;
}
typedef struct { char *name; char *(*grant)(const char *, const char *, const char *, gboolean *); } CfShareProvider;
static CfShareProvider share = {"stub-share", share_capability};
static GList *share_providers;
#include "production.c"

void fixture_reset(void)
{
    memset(counters, 0, sizeof(counters));
    now = 0; fail_at = timeout_at = deny_repo = managed = boundary_unavailable = 0;
    g_clear_pointer(&share_level, g_free);
    share_unavailable = 0;
    if (!providers) { providers = g_list_append(providers, &acl); providers = g_list_append(providers, &hook); }
    if (!share_providers) share_providers = g_list_append(share_providers, &share);
}
void fixture_fail_at(int at) { fail_at = at; }
void fixture_timeout_at(int at) { timeout_at = at; }
void fixture_deny_repo(int value) { deny_repo = value; }
void fixture_manage(int value) { managed = value; }
void fixture_boundary_unavailable(int value) { boundary_unavailable = value; }
void fixture_share_level(const char *level) { g_free(share_level); share_level = level ? g_strdup(level) : NULL; }
void fixture_share_unavailable(int value) { share_unavailable = value; }
int fixture_count(int index) { return counters[index]; }
static char *finish(char *result, GError *error)
{
    if (error) {
        g_assert_null(result);
        result = g_strdup("{\"rpc_error\":true}");
        g_error_free(error);
    }
    return result;
}
char *fixture_many(const char *request)
{
    GError *error = NULL;
    char *result = seafile_cf_check_permissions_many(request, &error);
    return finish(result, error);
}
char *fixture_scalar(const char *repo, const char *path, const char *user)
{
    GError *error = NULL;
    char *result = seafile_check_permission_by_path(repo, path, user, &error);
    return finish(result, error);
}
void fixture_free(void *value) { g_free(value); }
