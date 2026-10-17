#include "common.h"
#include "cf-v2-acl.h"
#include "seafile-session.h"

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
    int count = trans ? seaf_db_trans_foreach_selected_row(trans, query, found, &protected,
                            2, "string", repo_id, "string", repo_id) :
                       seaf_db_statement_foreach_row(session->db, query, found, &protected,
                            2, "string", repo_id, "string", repo_id);
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
        /* Virtual repositories need an origin/path proof; do not infer it from
         * an unprotected child. SQL failure is distinct from an empty result. */
        gboolean db_error=FALSE;
        gboolean virtual=seaf_db_trans_check_for_existence(guard->trans,
            "SELECT 1 FROM VirtualRepo WHERE repo_id=?", &db_error, 1, "string", repo);
        if (db_error || virtual || check(guard->trans,session,repo)!=0) goto failed;
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

static int authorize(struct _SeafileSession *session, json_t *request)
{
    int result=-1;
    jwt_t *jwt=NULL;
    CURL *curl=NULL;
    struct curl_slist *headers=NULL;
    char *token=NULL, *auth=NULL, *url=NULL, *payload=NULL;
    GString *body=g_string_new(NULL);
    json_t *reply=NULL;
    if (!session->seahub_pk || !*session->seahub_pk || !session->seahub_url) goto done;
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
    payload=json_dumps(request,JSON_COMPACT);
    if (!payload) goto done;
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,payload);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,5L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,response_body);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,body);
    if (curl_easy_perform(curl)!=CURLE_OK) goto done;
    long status=0;
    if (curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK || status!=200) goto done;
    reply=json_loadb(body->str,body->len,JSON_REJECT_DUPLICATES,NULL);
    if (json_is_true(json_object_get(reply,"allowed")) &&
        !g_strcmp0(json_string_value(json_object_get(reply,"contract")),"cf-acl-v1")) result=0;
done:
    if (curl) curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (jwt) jwt_free(jwt);
    json_decref(reply);
    g_free(token);g_free(auth);g_free(url);g_free(payload);g_string_free(body,TRUE);
    return result;
}

int cf_v2_acl_final_scoped(SeafDBTrans *trans, struct _SeafileSession *session,
    const char *repo_id, const char *old_id, const char *new_id, const char *actor)
{
    if (!trans) return -1;
    int state=cf_v2_acl_final(trans,session,repo_id);
    if (state==0) return 0;
    if (state!=-2) return -1;
    /* Distinguish protected scope from a SQL/config failure: neither grants.
     * The HTTP authority independently reads the same facts and denies failures. */
    if (!actor || !*actor || !old_id || !new_id) return -1;
    SeafRepo *repo=seaf_repo_manager_get_repo(session->repo_mgr,repo_id);
    if (!repo || repo->virtual_info) { if (repo) seaf_repo_unref(repo); return -1; }
    SeafCommit *old=seaf_commit_manager_get_commit(session->commit_mgr,repo_id,repo->version,old_id);
    SeafCommit *next=seaf_commit_manager_get_commit(session->commit_mgr,repo_id,repo->version,new_id);
    GList *changes=NULL;
    json_t *checks=json_array();
    json_t *request=json_object();
    int result=-1;
    if (!old || !next || diff_commit_roots(repo->store_id,repo->version,old->root_id,next->root_id,&changes,FALSE)<0) goto done;
    for (GList *p=changes;p;p=p->next) {
        DiffEntry *delta=p->data;
        if (json_array_size(checks)>=4096 || (delta->status!=DIFF_STATUS_ADDED && delta->status!=DIFF_STATUS_MODIFIED)) goto done;
        char *path=g_strconcat("/",delta->name,NULL);
        if (!canonical(path)) {g_free(path);goto done;}
        json_array_append_new(checks,json_pack("{s:s,s:s,s:s}","path",path,"kind","file","action","write"));
        g_free(path);
    }
    if (!json_array_size(checks)) goto done;
    json_object_set_new(request,"repo_id",json_string(repo_id));
    json_object_set_new(request,"username",json_string(actor));
    json_object_set(request,"checks",checks);
    result=authorize(session,request);
done:
    json_decref(request);json_decref(checks);
    g_list_free_full(changes,(GDestroyNotify)diff_entry_free);
    if (old) seaf_commit_unref(old);
    if (next) seaf_commit_unref(next);
    seaf_repo_unref(repo);
    return result;
}
