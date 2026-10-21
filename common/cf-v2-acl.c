#include "common.h"
#include "cf-v2-acl.h"
#include "seafile-session.h"
#include "repo-mgr.h"

static const char *query =
    "SELECT 1 FROM cf_dir_acl WHERE repo_id=? OR repo_id IN "
    "(SELECT origin_repo FROM VirtualRepo WHERE repo_id=?) LIMIT 1";

static gboolean found(SeafDBRow *row, void *data)
{
    *(gboolean *)data = TRUE;
    return FALSE;
}

static int check(SeafDBTrans *trans, struct _SeafileSession *session, const char *repo_id)
{
    if (!session || !session->db || !repo_id ||
        seaf_db_type(session->db) != SEAF_DB_TYPE_MYSQL) return -1;
    gboolean protected = FALSE;
    /* A prior barrier read may have established a REPEATABLE READ snapshot.
     * Use a locking/current read after Branch, never that older snapshot. */
    char *current_query = trans ? g_strconcat(query, " FOR UPDATE", NULL) : NULL;
    int count = trans ? seaf_db_trans_foreach_selected_row(trans, current_query, found, &protected,
                            2, "string", repo_id, "string", repo_id) :
                       seaf_db_statement_foreach_row(session->db, query, found, &protected,
                            2, "string", repo_id, "string", repo_id);
    g_free(current_query);
    return count < 0 ? -1 : protected ? -2 : 0;
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
