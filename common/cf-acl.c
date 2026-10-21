/* CloudFile V2 native permission seam; no library enrollment or class. */
#include "common.h"
#include <string.h>
#include <jansson.h>
#include "log.h"
#include "seafile-session.h"
#include "seaf-db.h"
#include "cf-ext.h"
#include "cf-acl.h"
#include "cf-v2-acl.h"

static gboolean cf_acl_on = FALSE;
void cf_acl_init (void)
{
    /* V2 directory policy is built in, not activated by a legacy switch.
     * Register even when the Hub is offline: failures must not grant access. */
    cf_acl_on = (seaf != NULL);
    if (cf_acl_on)
        cf_ext_register ("v2-path-authority", cf_acl_apply,
                         cf_acl_filter_dirents, cf_acl_find_restricted_path);
}
gboolean cf_acl_enabled (void) { return cf_acl_on; }

/* The trusted RPC caller supplies username, repository and the exact path.
 * Hub Authority resolves CE qualification, ACL inheritance and share roots.
 * The C seam consumes the same internal check as native final-commit. */
static char *
v2_effective_read (const char *repo, const char *path, const char *user,
                   gboolean *unavailable)
{
    if (!repo || !path || !user || !*user) return NULL;
    json_t *request=json_object(), *checks=json_array();
    json_t *check=json_pack("{s:s,s:s,s:s}","path",path,
                            "kind","dir","action","read");
    json_t *reply=NULL;
    GString *body=g_string_new(NULL);
    char *payload=NULL, *result=NULL;
    if (!request || !checks || !check ||
        json_array_append(checks,check) ||
        json_object_set_new(request,"repo_id",json_string(repo)) ||
        json_object_set_new(request,"username",json_string(user)) ||
        json_object_set(request,"checks",checks)) {
        if(unavailable)*unavailable=TRUE;
        goto done;
    }
    payload=json_dumps(request,JSON_COMPACT);
    if(!payload) { if(unavailable)*unavailable=TRUE; goto done; }
    long status=cf_v2_acl_check_json(seaf,payload,body);
    if(status!=200) {
        if(unavailable && (status<0 || status>=500))*unavailable=TRUE;
        goto done;
    }
    reply=json_loadb(body->str,body->len,JSON_REJECT_DUPLICATES,NULL);
    json_t *permissions=json_object_get(reply,"permissions");
    if (!json_is_true(json_object_get(reply,"allowed")) ||
        g_strcmp0(json_string_value(json_object_get(reply,"contract")),"cf-acl-v1") ||
        !json_is_array(permissions) || json_array_size(permissions)!=1) {
        if(unavailable)*unavailable=TRUE;
        goto done;
    }
    const char *level=json_string_value(json_array_get(permissions,0));
    if(level && (!strcmp(level,"r") || !strcmp(level,"rw")))
        result=g_strdup(level);
    else if(unavailable)*unavailable=TRUE;
done:
    json_decref(reply);json_decref(request);json_decref(checks);json_decref(check);
    g_free(payload);g_string_free(body,TRUE);
    return result;
}
char *
cf_acl_apply (const char *repo, const char *path, const char *user,
              const char *native_perm, gboolean *unavailable)
{
    if(unavailable)*unavailable=FALSE;
    if(!native_perm) return NULL;
    int scope=cf_v2_acl_unscoped(seaf,repo);
    if(scope==0) return g_strdup(native_perm);
    if(scope!=-2) { if(unavailable)*unavailable=TRUE; return NULL; }
    return v2_effective_read(repo,path,user,unavailable);
}

/* Every visible child is checked on its actual path. A sparse ACL on /A may
 * hide /A but must not hide the /B siblings or the entire repository. */
GList *
cf_acl_filter_dirents (const char *repo, const char *dir,
                       const char *user, GList *dirents)
{
    if(!cf_acl_on || !dirents) return dirents;
    int scope=cf_v2_acl_unscoped(seaf,repo);
    if(scope==0) return dirents;
    if(scope!=-2) { g_list_free_full(dirents,g_object_unref); return NULL; }
    GList *visible=NULL;
    for(GList *p=dirents;p;p=p->next) {
        GObject *item=p->data;
        char *name=NULL,*native=NULL;
        g_object_get(item,"obj_name",&name,"permission",&native,NULL);
        char *path=name && *name && !strchr(name,'/') &&
                   strcmp(name,".") && strcmp(name,"..") ?
                   g_build_filename(dir,name,NULL) : NULL;
        gboolean unavailable=FALSE;
        char *perm=path ? cf_acl_apply(repo,path,user,native,&unavailable) : NULL;
        if(perm) {
            g_object_set(item,"permission",perm,NULL);
            visible=g_list_prepend(visible,g_object_ref(item));
        }
        g_free(perm);g_free(path);g_free(name);g_free(native);
    }
    g_list_free_full(dirents,g_object_unref);
    return g_list_reverse(visible);
}

/* Legacy whole-tree export/sync has no per-child authorization. Only rules
 * affecting the requested subtree can make this operation ineligible. */
char *
cf_acl_find_restricted_path (const char *repo, const char *path,
                             const char *user, const char *native_perm)
{
    if(!repo || !path || !native_perm) return NULL;
    gboolean unavailable=FALSE;
    char *perm=cf_acl_apply(repo,path,user,native_perm,&unavailable);
    if(!perm) return g_strdup(path);
    gboolean too_narrow=(!strcmp(native_perm,"rw") && strcmp(perm,"rw"));
    g_free(perm);
    if(too_narrow || !seaf || !seaf->db) return g_strdup(path);
    gboolean db_error=FALSE;
    gboolean descendants=seaf_db_statement_exists(seaf->db,
        "SELECT 1 FROM cf_dir_acl WHERE repo_id=? AND ("
        "(?='/' AND path<>'/') OR path LIKE CONCAT(?,'/%')) LIMIT 1",
        &db_error,3,"string",repo,"string",path,"string",path);
    return (db_error || descendants) ? g_strdup(path) : NULL;
}
