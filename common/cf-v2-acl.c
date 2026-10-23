#include "common.h"
#include "cf-v2-acl.h"
#include "seafile-session.h"
#include "repo-mgr.h"

/* Final ACL proof must use the (repo_id, path_hash, ...) PRIMARY prefix.
 * The old OR/IN VirtualRepo query chose acl_shared_subject, scanning and
 * X-locking unrelated libraries under REPEATABLE READ. No new index needed. */
static const char *query =
    "SELECT 1 FROM cf_dir_acl FORCE INDEX (PRIMARY) WHERE repo_id=? LIMIT 1";
static const char *origin_query =
    "SELECT origin_repo FROM VirtualRepo WHERE repo_id=?";

static gboolean found(SeafDBRow *row, void *data)
{
    *(gboolean *)data = TRUE;
    return FALSE;
}

static gboolean origin_found(SeafDBRow *row, void *data)
{
    const char *origin = seaf_db_row_get_column_text(row, 0);
    *(char **)data = g_strdup(origin);
    return FALSE;
}

static int check_scope(SeafDBTrans *trans, struct _SeafileSession *session,
                       const char *repo_id)
{
    gboolean protected = FALSE;
    /* A prior barrier read may have established a REPEATABLE READ snapshot.
     * Use a locking/current read after Branch, never that older snapshot. */
    char *current_query = trans ? g_strconcat(query, " FOR UPDATE", NULL) : NULL;
    int count = trans ? seaf_db_trans_foreach_selected_row(trans, current_query, found, &protected,
                            1, "string", repo_id) :
                       seaf_db_statement_foreach_row(session->db, query, found, &protected,
                            1, "string", repo_id);
    g_free(current_query);
    return count < 0 ? -1 : protected ? -2 : 0;
}

static int check(SeafDBTrans *trans, struct _SeafileSession *session, const char *repo_id)
{
    if (!session || !session->db || !repo_id ||
        seaf_db_type(session->db) != SEAF_DB_TYPE_MYSQL) return -1;

    int result = check_scope(trans, session, repo_id);
    if (result != 0) return result;

    /* Preserve CE VirtualRepo origin qualification without combining two
     * repository keys in a single optimizer-dependent ACL locking scan.
     * VirtualRepo.repo_id already has a unique index. Do not infer an
     * unprotected scope from a failed or malformed origin lookup. */
    char *origin = NULL;
    int count = trans ? seaf_db_trans_foreach_selected_row(trans, origin_query,
                            origin_found, &origin, 1, "string", repo_id) :
                       seaf_db_statement_foreach_row(session->db, origin_query,
                            origin_found, &origin, 1, "string", repo_id);
    if (count < 0 || count > 1 ||
        (count == 1 && (!origin || strlen(origin) != 36 ||
                        strcmp(origin, repo_id) == 0))) {
        g_free(origin);
        return -1;
    }
    if (count == 1)
        result = check_scope(trans, session, origin);
    g_free(origin);
    return result;
}

/* Native transfer and Hub ACL mutations share per-library advisory locks.
 * Keep the dedicated connection until synchronous publication completes: an
 * empty ACL snapshot alone would race a newly installed source restriction. */
struct CfV2ACLTransfer {
    SeafDBTrans *trans;
    char *repos[2];
};

static gboolean lock_result(SeafDBRow *row, void *data)
{
    *(int *)data = seaf_db_row_get_column_int(row, 0);
    return FALSE;
}

static int transfer_lock(SeafDBTrans *trans, const char *repo, gboolean release)
{
    int result = -1;
    const char *sql = release ?
        "SELECT RELEASE_LOCK(CONCAT('cfv2:t:',LEFT(SHA2(CONCAT(DATABASE(),':',?),256),56)))" :
        "SELECT GET_LOCK(CONCAT('cfv2:t:',LEFT(SHA2(CONCAT(DATABASE(),':',?),256),56)),5)";
    int count = seaf_db_trans_foreach_selected_row(trans, sql, lock_result, &result, 1, "string", repo);
    return count < 0 || result != 1 ? -1 : 0;
}

void cf_v2_acl_transfer_free(CfV2ACLTransfer *guard)
{
    if (!guard) return;
    if (guard->trans) {
        /* GET_LOCK is connection-scoped, so rollback alone would leak it into
         * the native pool. Always release before returning the connection. */
        for (int i=1; i>=0; --i)
            if (guard->repos[i]) transfer_lock(guard->trans, guard->repos[i], TRUE);
        seaf_db_rollback(guard->trans);
        seaf_db_trans_close(guard->trans);
    }
    g_free(guard->repos[0]);g_free(guard->repos[1]);g_free(guard);
}

CfV2ACLTransfer *cf_v2_acl_transfer_begin(struct _SeafileSession *session,
                                         const char *source, const char *destination)
{
    if (!session || !session->db || !source || !destination ||
        seaf_db_type(session->db) != SEAF_DB_TYPE_MYSQL) return NULL;
    CfV2ACLTransfer *guard = g_new0(CfV2ACLTransfer, 1);
    guard->trans = seaf_db_begin_transaction(session->db);
    if (!guard->trans) goto failed;
    const char *first = strcmp(source,destination)<=0 ? source : destination;
    const char *second = strcmp(source,destination)<=0 ? destination : source;
    if (transfer_lock(guard->trans,first,FALSE)<0) goto failed;
    guard->repos[0]=g_strdup(first);
    if (strcmp(first,second)) {
        if (transfer_lock(guard->trans,second,FALSE)<0) goto failed;
        guard->repos[1]=g_strdup(second);
    }
    for (int i=0;i<2;++i) {
        const char *repo=guard->repos[i];
        if (!repo) continue;
        /* CE virtual libraries remain usable when neither library nor origin
         * carries ACL restrictions. Scoped restrictions still need proof. */
        if (check(guard->trans,session,repo)!=0) goto failed;
    }
    return guard;
failed:
    cf_v2_acl_transfer_free(guard);
    return NULL;
}

int cf_v2_acl_unscoped(struct _SeafileSession *session, const char *repo_id)
{
    return check(NULL, session, repo_id);
}

int cf_v2_acl_final(SeafDBTrans *trans, struct _SeafileSession *session, const char *repo_id)
{
    if (!trans) return -1;
    /* Called inside Branch(master) transaction. No authenticated operation
     * context exists at this legacy seam: commit author is NOT identity proof. */
    return check(trans, session, repo_id);
}

#include <curl/curl.h>
#include <jansson.h>
#include <jwt.h>
#include "commit-mgr.h"
#include "diff-simple.h"
#include "repo-mgr.h"

static gboolean canonical(const char *path);

/* Public links are independently authenticated by Seahub. Check whether
 * ACL policy intersects the ACTUAL linked path or exported subtree; an ACL
 * on a sibling is not a repository-wide block. CE virtual links use the
 * existing native origin/path mapping. */
int cf_v2_acl_public_link_scope(struct _SeafileSession *session,
                                const char *repo, const char *path,
                                gboolean subtree)
{
    if (!repo || strlen(repo)!=36 || !canonical(path))
        return 1;
    if (!session || !session->db || seaf_db_type(session->db)!=SEAF_DB_TYPE_MYSQL)
        return -1;
    gboolean failed=FALSE;
    gboolean virtual=seaf_db_statement_exists(session->db,
        "SELECT 1 FROM VirtualRepo WHERE repo_id=?",
        &failed, 1, "string", repo);
    if (failed) return -1;
    if (virtual) {
        SeafVirtRepo *info = seaf_repo_manager_get_virtual_repo_info(session->repo_mgr, repo);
        if (!info) return -1;
        if (!canonical(info->path) || !strcmp(info->origin_repo_id, repo)) {
            seaf_virtual_repo_info_free(info);
            return -1;
        }
        char *mapped = !strcmp(path, "/") ? g_strdup(info->path) :
            g_build_filename(info->path, path + 1, NULL);
        int result = cf_v2_acl_public_link_scope(session, info->origin_repo_id, mapped, subtree);
        g_free(mapped);
        seaf_virtual_repo_info_free(info);
        return result;
    }
    const char *sql =
        "SELECT 1 FROM cf_dir_acl WHERE repo_id=? AND "
        "(path='/' OR path=? OR ? LIKE CONCAT(path,'/%') OR "
        "(? AND ((?='/' AND path<>'/') OR path LIKE CONCAT(?,'/%')))) LIMIT 1";
    gboolean intersects=seaf_db_statement_exists(session->db,sql,&failed,6,
        "string",repo,"string",path,"string",path,
        "int",subtree?1:0,"string",path,"string",path);
    if (failed) return -1;
    return intersects ? 1 : 0;
}

static gboolean canonical(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) > 4096 || !g_utf8_validate(path, -1, NULL) ||
        strchr(path, '\\') || strstr(path, "//") || (strlen(path)>1 && path[strlen(path)-1]=='/')) return FALSE;
    for (const unsigned char *p=(const unsigned char *)path; *p; ++p)
        if (*p < 32 || *p == 127) return FALSE;
    gchar **parts=g_strsplit(path,"/",-1);
    gboolean valid=TRUE;
    for (int i=0;parts[i];++i)
        if (!strcmp(parts[i],".") || !strcmp(parts[i],"..")) valid=FALSE;
    g_strfreev(parts);
    return valid;
}

static size_t response_body(char *data, size_t size, size_t n, void *opaque)
{
    GString *body=opaque;
    if (size && n > 4096/size) return 0;
    size_t length=size*n;
    if (body->len+length>4096) return 0;
    g_string_append_len(body,data,length);
    return length;
}

/*
 * Transport only: one POST to the Hub's internal V2 check endpoint.
 *
 * Shared transport so the directory-share qualification (cf-v2-share.c)
 * reuses this channel instead of growing a second curl block with its own JWT
 * lifetime rules -- two copies of an internal-auth handshake are one copy too
 * many, and the one that drifts is always the less exercised one.
 *
 * Returns the HTTP status, or -1 when the channel itself is unusable: no key or
 * url, JWT failure, transport error, oversized or truncated reply. @body is
 * reset, and holds the reply on 200.
 */
long
cf_v2_acl_check_json(struct _SeafileSession *session, const char *payload, GString *body)
{
    long status=-1;
    jwt_t *jwt=NULL;
    CURL *curl=NULL;
    struct curl_slist *headers=NULL;
    char *token=NULL, *auth=NULL, *url=NULL;
    if (!session || !payload || !body) return -1;
    g_string_truncate(body,0);
    if (!session->seahub_pk || !*session->seahub_pk || !session->seahub_url) return -1;
    if (jwt_new(&jwt) || jwt_add_grant_bool(jwt,"is_internal",TRUE) ||
        jwt_add_grant_int(jwt,"exp",time(NULL)+60) ||
        jwt_set_alg(jwt,JWT_ALG_HS256,(unsigned char *)session->seahub_pk,strlen(session->seahub_pk))) goto done;
    token=jwt_encode_str(jwt);
    if (!token) goto done;
    curl=curl_easy_init();
    if (!curl) goto done;
    url=g_strdup_printf("%s/cloudfile/acl/check/",session->seahub_url);
    auth=g_strdup_printf("Authorization: Token %s",token);
    headers=curl_slist_append(headers,auth);
    headers=curl_slist_append(headers,"Content-Type: application/json");
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,payload);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,5L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,response_body);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,body);
    if (curl_easy_perform(curl)!=CURLE_OK) goto done;
    if (curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK) status=-1;
done:
    if (curl) curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (jwt) jwt_free(jwt);
    g_free(token);g_free(auth);g_free(url);
    return status;
}

/* ---------------------------------------------------- scoped final check */

/* POST the change description to the Hub V2 authority. 0 only when the reply
 * is a positive cf-acl-v1 verdict; every transport or parse problem denies. */
static int
authorize_change (struct _SeafileSession *session, json_t *request)
{
    int result = -1;
    char *payload = NULL;
    GString *body = g_string_new (NULL);
    json_t *reply = NULL;

    payload = json_dumps (request, JSON_COMPACT);
    if (!payload) goto done;
    if (cf_v2_acl_check_json (session, payload, body) != 200) goto done;
    reply = json_loadb (body->str, body->len, JSON_REJECT_DUPLICATES, NULL);
    if (reply &&
        json_is_true (json_object_get (reply, "allowed")) &&
        !g_strcmp0 (json_string_value (json_object_get (reply, "contract")),
                    "cf-acl-v1"))
        result = 0;

done:
    if (reply) json_decref (reply);
    g_free (payload);
    g_string_free (body, TRUE);
    return result;
}

/* Folded-directory diffs never reach us (callers pass fold_dir_diff=FALSE),
 * so the status letter alone tells files from directories. */
static gboolean
scoped_entry_is_dir (DiffEntry *de)
{
    return de->status == DIFF_STATUS_DIR_ADDED ||
           de->status == DIFF_STATUS_DIR_DELETED ||
           de->status == DIFF_STATUS_DIR_RENAMED;
}

static gboolean
scoped_status_supported (DiffEntry *de)
{
    switch (de->status) {
    case DIFF_STATUS_ADDED:
    case DIFF_STATUS_MODIFIED:
    case DIFF_STATUS_DELETED:
    case DIFF_STATUS_RENAMED:
    case DIFF_STATUS_DIR_ADDED:
    case DIFF_STATUS_DIR_DELETED:
    case DIFF_STATUS_DIR_RENAMED:
        return TRUE;
    default:
        return FALSE;
    }
}

int
cf_v2_acl_final_scoped (SeafDBTrans *trans, struct _SeafileSession *session,
                        const char *repo_id, const char *old_id,
                        const char *new_id, const char *actor)
{
    if (!trans) return -1;

    int state = cf_v2_acl_final (trans, session, repo_id);
    if (state == 0) return 0;
    if (state != -2) return -1;

    /* Protected scope: qualify the concrete change. A missing actor or an
     * unreadable commit pair never grants. The Hub authority re-reads the
     * V2 store per path, so a forged commit author still needs real write
     * permission on every touched path; it cannot borrow another user's
     * grants. Fail closed on every ambiguity. */
    if (!actor || !*actor || !old_id || !new_id) return -1;

    SeafRepo *repo = seaf_repo_manager_get_repo (session->repo_mgr, repo_id);
    if (!repo || repo->virtual_info) {
        if (repo) seaf_repo_unref (repo);
        return -1;
    }

    SeafCommit *old = seaf_commit_manager_get_commit (session->commit_mgr,
                                                      repo_id, repo->version,
                                                      old_id);
    SeafCommit *next = seaf_commit_manager_get_commit (session->commit_mgr,
                                                       repo_id, repo->version,
                                                       new_id);
    GList *changes = NULL;
    json_t *checks = json_array ();
    json_t *request = json_object ();
    int result = -1;

    if (!old || !next ||
        diff_commit_roots (repo->store_id, repo->version,
                           old->root_id, next->root_id,
                           &changes, FALSE) < 0)
        goto done;

    for (GList *p = changes; p; p = p->next) {
        DiffEntry *delta = p->data;
        if (json_array_size (checks) >= 4096 ||
            !scoped_status_supported (delta))
            goto done;

        const char *kind = scoped_entry_is_dir (delta) ? "dir" : "file";
        const char *target = delta->name;

        /* Rename writes the new entry; the source is checked separately
         * below because removing the old name mutates its parent too. */
        if ((delta->status == DIFF_STATUS_RENAMED ||
             delta->status == DIFF_STATUS_DIR_RENAMED) &&
            delta->new_name && *delta->new_name)
            target = delta->new_name;

        char *path = g_strconcat ("/", target, NULL);
        if (!canonical (path)) { g_free (path); goto done; }
        json_array_append_new (checks,
                               json_pack ("{s:s,s:s,s:s}",
                                          "path", path,
                                          "kind", kind,
                                          "action", "write"));
        g_free (path);

        if ((delta->status == DIFF_STATUS_RENAMED ||
             delta->status == DIFF_STATUS_DIR_RENAMED) &&
            delta->new_name && *delta->new_name &&
            strcmp (delta->name, delta->new_name) != 0) {
            char *src = g_strconcat ("/", delta->name, NULL);
            if (!canonical (src)) { g_free (src); goto done; }
            json_array_append_new (checks,
                                   json_pack ("{s:s,s:s,s:s}",
                                              "path", src,
                                              "kind", kind,
                                              "action", "write"));
            g_free (src);
        }
    }

    if (!json_array_size (checks)) goto done;

    json_object_set_new (request, "repo_id", json_string (repo_id));
    json_object_set_new (request, "username", json_string (actor));
    json_object_set_new (request, "checks", checks);
    checks = NULL;  /* ownership moved into request */

    result = authorize_change (session, request);

done:
    if (request) json_decref (request);
    if (checks) json_decref (checks);
    g_list_free_full (changes, (GDestroyNotify)diff_entry_free);
    if (old) seaf_commit_unref (old);
    if (next) seaf_commit_unref (next);
    seaf_repo_unref (repo);
    return result;
}
