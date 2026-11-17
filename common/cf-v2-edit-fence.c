/* CF-EDIT V2: deny unscoped native publications touching reserved paths.
 *
 * The candidate diff is prepared before Branch CAS. Only current Guard rows
 * are locked and checked after Branch(master) in the final short transaction.
 * Checkout writers use the same Branch -> Guard lock order.
 * This is a fail-closed fence, NOT an authorization path for Commit/Checkin.
 */
#include "cf-v2-edit-fence.h"
#include "common.h"
#include "commit-mgr.h"
#include "diff-simple.h"
#include "fs-mgr.h"
#include "log.h"
#include "repo-mgr.h"
#include "seafile-session.h"

typedef struct {
    GPtrArray *paths;
    gboolean invalid;
} GuardPaths;

static gboolean
read_guard_path(SeafDBRow *row, void *data)
{
    GuardPaths *state = data;
    const char *path = seaf_db_row_get_column_text(row, 0);
    if (!path || path[0] != '/' || strlen(path) > 4096 || !g_utf8_validate(path, -1, NULL)) {
        state->invalid = TRUE;
        return FALSE;
    }
    if (state->paths->len >= 8192) {
        state->invalid = TRUE;
        return FALSE;
    }
    g_ptr_array_add(state->paths, g_strdup(path));
    return TRUE;
}

static gboolean
path_touches(const char *changed, const char *guarded)
{
    if (!changed || !*changed)
        return FALSE;
    const char *name = changed[0] == '/' ? changed + 1 : changed;
    char *absolute = g_strconcat("/", name, NULL);
    gboolean touches = !strcmp(absolute, guarded) ||
        (g_str_has_prefix(guarded, absolute) && guarded[strlen(absolute)] == '/');
    g_free(absolute);
    return touches;
}

static gboolean
canonical_repo_id(const char *id)
{
    if (!id || strlen(id) != 36)
        return FALSE;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-')
                return FALSE;
        } else if (!g_ascii_isdigit(id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return FALSE;
        }
    }
    return TRUE;
}

/* 修改原因（2026-11-16）：Docker 引导仍会生成 protection_repo_ids /
 * checkout_repo_ids，但运行期只查 cf_repo_feature。运维看到配置还在就会认为
 * 功能已开启；这里在进程首次判定库级策略时告警一次，把静默失效变可见。
 * 只告警，不阻断：配置项已无判定权，报错只会制造与功能无关的启动故障。 */
static void
warn_legacy_repo_scope (struct _SeafileSession *session)
{
    static gsize warned = 0;
    if (!g_once_init_enter (&warned))
        return;
    const char *keys[] = { "protection_repo_ids", "checkout_repo_ids" };
    for (guint i = 0; i < G_N_ELEMENTS (keys); ++i) {
        char *raw = (session && session->config) ?
            g_key_file_get_string (session->config, "cloudfile", keys[i], NULL) : NULL;
        if (raw && *g_strstrip (raw))
            seaf_warning ("CloudFile: %s is a legacy migration input and is IGNORED "
                          "at runtime. Per-library capabilities are stored in "
                          "cf_repo_feature; use POST "
                          "/api/v2.1/admin/cloudfile/repos/<uuid>/features/.\n",
                          keys[i]);
        g_free (raw);
    }
    g_once_init_leave (&warned, 1);
}

/* A final locking read serializes a feature change with publication. Never
 * retain process-wide allowlists or interpret an unavailable policy as OFF. */
static int
repo_feature(struct _SeafileSession *session, SeafDBTrans *trans,
             const char *repo_id, const char *feature)
{
    gboolean error = FALSE;
    if (!session || !session->db || !canonical_repo_id(repo_id))
        return -1;
    gboolean present = trans ? seaf_db_trans_check_for_existence(trans,
        "SELECT 1 FROM cf_repo_feature WHERE repo_id=? AND feature=? LOCK IN SHARE MODE",
        &error, 2, "string", repo_id, "string", feature) :
        seaf_db_statement_exists(session->db,
        "SELECT 1 FROM cf_repo_feature WHERE repo_id=? AND feature=?",
        &error, 2, "string", repo_id, "string", feature);
    return error ? -1 : present;
}

int
cf_v2_file_protection_policy(struct _SeafileSession *session, const char *repo_id)
{
    warn_legacy_repo_scope (session);
    return repo_feature(session, NULL, repo_id, "protection");
}

static int
checkout_policy(struct _SeafileSession *session, SeafDBTrans *trans, const char *repo_id)
{
    /* Dynamic Checkout is authorized exclusively by cf_repo_feature.
     * The former global startup switch is a migration hint, not a security
     * gate; final reads still serialize under the Branch(master) lock. */
    return repo_feature(session, trans, repo_id, "checkout");
}

int
cf_v2_edit_repo_checkout(struct _SeafileSession *session, const char *repo_id)
{
    return checkout_policy(session, NULL, repo_id);
}

/* Schema is validated on guarded paths only. Its absence is a failure for an
 * OPTIONAL repository, never a reason to bypass an active Checkout. */
static gsize guard_schema_state = 0; /* 0=unread, 1=absent, 2=installed, 3=error */

static gboolean
guard_table_found(SeafDBRow *row, void *data)
{
    (void)row;
    *(gboolean *)data = TRUE;
    return FALSE;
}

static int
guard_available(struct _SeafileSession *session)
{
    if (!session || !session->db ||
        seaf_db_type(session->db) != SEAF_DB_TYPE_MYSQL)
        return -1;
    if (g_once_init_enter(&guard_schema_state)) {
        gboolean found = FALSE;
        int count = seaf_db_statement_foreach_row(session->db,
            "SELECT 1 FROM information_schema.TABLES "
            "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_guard' LIMIT 1",
            guard_table_found, &found, 0);
        g_once_init_leave(&guard_schema_state, count < 0 ? 3 : found ? 2 : 1);
    }
    return guard_schema_state == 2 ? 1 : -1;
}

/* The Guard policy is OFF by default. For OPTIONAL libraries, resolve the
 * candidate commit's changed paths before opening the final Branch transaction.
 * DiffEntry objects are immutable for this candidate: a racing HEAD is caught
 * by the final Branch CAS, and new Guard reservations are read under Branch. */
struct _CfV2EditPrepared {
    char *repo_id;
    char *old_id;
    char *new_id;
    GList *changes;
    int checkout, protection, origin_protection;
    char *origin;
    char *prefix;
    GHashTable *candidate_ids;
};

/* Immutable candidate proofs for origin-scoped virtual publications. */
static char *protection_prefix(const char *path);

static gboolean
virtual_mapping(SeafDBRow *row, void *data)
{
    CfV2EditPrepared *p = data;
    p->origin = g_strdup(seaf_db_row_get_column_text(row, 0));
    p->prefix = g_strdup(seaf_db_row_get_column_text(row, 1));
    return FALSE;
}

typedef struct {
    CfV2EditPrepared *prepared;
    struct _SeafileSession *session;
    const char *root;
    int version;
    gboolean final;
    gboolean invalid;
    guint count;
} VirtualProof;

static gboolean
virtual_protected_row(SeafDBRow *row, void *data)
{
    VirtualProof *proof = data;
    CfV2EditPrepared *p = proof->prepared;
    const char *path = seaf_db_row_get_column_text(row, 0);
    const char *id = seaf_db_row_get_column_text(row, 1);
    if (!path || !id || ++proof->count > 8192 ||
        !g_str_has_prefix(path, p->prefix) || path[strlen(p->prefix)] != '/') {
        proof->invalid = TRUE;
        return FALSE;
    }
    if (proof->final) {
        const char *candidate = g_hash_table_lookup(p->candidate_ids, path);
        if (!candidate || !*candidate || strcmp(candidate, id)) {
            proof->invalid = TRUE;
            return FALSE;
        }
    } else if (!g_hash_table_contains(p->candidate_ids, path)) {
        if (g_hash_table_size(p->candidate_ids) >= 8192) { proof->invalid = TRUE; return FALSE; }
        GError *error = NULL;
        char *candidate = seaf_fs_manager_get_seafile_id_by_path(
            proof->session->fs_mgr, p->origin, proof->version, proof->root,
            path + strlen(p->prefix), &error);
        g_clear_error(&error);
        g_hash_table_insert(p->candidate_ids, g_strdup(path), candidate ? candidate : g_strdup(""));
    }
    return TRUE;
}

static int
virtual_rows(SeafDBTrans *trans, VirtualProof *proof, const char *changed)
{
    if (!changed || !*changed) return 0;
    CfV2EditPrepared *p = proof->prepared;
    char *path = g_strconcat(p->prefix, "/", changed[0] == '/' ? changed + 1 : changed, NULL);
    char *pattern = protection_prefix(path);
    const char *query = "SELECT path,file_id FROM cf_file_protection "
        "WHERE repo_id=? AND (path=? OR path LIKE ? ESCAPE '!') LIMIT 8193";
    char *sql = g_strconcat(query, trans ? " FOR UPDATE" : "", NULL);
    proof->count = 0;
    int n = trans ? seaf_db_trans_foreach_selected_row(trans, sql,
        virtual_protected_row, proof, 3, "string", p->origin, "string", path, "string", pattern)
        : seaf_db_statement_foreach_row(proof->session->db, sql,
        virtual_protected_row, proof, 3, "string", p->origin, "string", path, "string", pattern);
    g_free(sql); g_free(pattern); g_free(path);
    /* 修改原因（2026-11-17）：1=候选提交动了受保护原件，-1=SQL/遍历故障。
     * 调用方据此区分"文件已定版"与"策略不可用"。 */
    if (n < 0) return -1;
    return proof->invalid ? 1 : 0;
}

static gboolean
virtual_origin_head(SeafDBRow *row, void *data)
{
    const char *head = seaf_db_row_get_column_text(row, 0);
    *(gboolean *)data = head && strlen(head) == 40;
    return FALSE;
}

int
cf_v2_edit_lock_origin(SeafDBTrans *trans, const CfV2EditPrepared *prepared)
{
    if (!prepared) return -1;
    if (!prepared->origin) return 0;
    gboolean found = FALSE;
    int n = seaf_db_trans_foreach_selected_row(trans,
        "SELECT commit_id FROM Branch WHERE name='master' AND repo_id=? FOR UPDATE",
        virtual_origin_head, &found, 1, "string", prepared->origin);
    if (n != 1 || !found) return -1;
    CfV2EditPrepared current = {0};
    n = seaf_db_trans_foreach_selected_row(trans,
        "SELECT origin_repo,path FROM VirtualRepo WHERE repo_id=? FOR UPDATE",
        virtual_mapping, &current, 1, "string", prepared->repo_id);
    int result = n == 1 && !g_strcmp0(current.origin, prepared->origin) &&
        !g_strcmp0(current.prefix, prepared->prefix) ? 0 : -1;
    g_free(current.origin); g_free(current.prefix);
    return result;
}

void
cf_v2_edit_prepared_free(CfV2EditPrepared *prepared)
{
    if (!prepared)
        return;
    g_list_free_full(prepared->changes, (GDestroyNotify)diff_entry_free);
    g_free(prepared->origin);
    g_free(prepared->prefix);
    if (prepared->candidate_ids) g_hash_table_destroy(prepared->candidate_ids);
    g_free(prepared->repo_id);
    g_free(prepared->old_id);
    g_free(prepared->new_id);
    g_free(prepared);
}

CfV2EditPrepared *
cf_v2_edit_prepare_unscoped_branch(struct _SeafileSession *session,
                                   const char *repo_id,
                                   const char *old_commit_id,
                                   const char *new_commit_id)
{
    if (!session || !repo_id || !old_commit_id || !new_commit_id)
        return NULL;
    int policy = cf_v2_edit_repo_checkout(session, repo_id);
    if (policy < 0)
        return NULL;
    CfV2EditPrepared *prepared = g_new0(CfV2EditPrepared, 1);
    prepared->repo_id = g_strdup(repo_id);
    prepared->old_id = g_strdup(old_commit_id);
    prepared->new_id = g_strdup(new_commit_id);
    int protection = cf_v2_file_protection_policy(session, repo_id);
    if (policy < 0 || protection < 0)
        goto fail;
    prepared->checkout = policy;
    prepared->protection = protection;
    {
        int n = seaf_db_statement_foreach_row(session->db,
            "SELECT origin_repo,path FROM VirtualRepo WHERE repo_id=?",
            virtual_mapping, prepared, 1, "string", repo_id);
        if (n < 0) goto fail;
        if (n > 0) {
            if (!canonical_repo_id(prepared->origin) || !prepared->prefix ||
                prepared->prefix[0] != '/' || !strcmp(prepared->origin, repo_id) ||
                strlen(prepared->prefix) < 2 || g_str_has_suffix(prepared->prefix, "/")) goto fail;
            prepared->origin_protection = cf_v2_file_protection_policy(session, prepared->origin);
            if (prepared->origin_protection < 0) goto fail;
            if (prepared->origin_protection) {
                prepared->candidate_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            }
        }
    }
    if ((policy == 0 && protection == 0 && !prepared->origin_protection) || !strcmp(old_commit_id, new_commit_id))
        return prepared;
    if (policy == 1 && guard_available(session) <= 0)
        goto fail;
    SeafRepo *repo = seaf_repo_manager_get_repo(session->repo_mgr, repo_id);
    if (!repo)
        goto fail;
    SeafCommit *old_commit = seaf_commit_manager_get_commit(
        session->commit_mgr, repo_id, repo->version, old_commit_id);
    SeafCommit *new_commit = seaf_commit_manager_get_commit(
        session->commit_mgr, repo_id, repo->version, new_commit_id);
    int result = (old_commit && new_commit)
        ? diff_commit_roots(repo->store_id, repo->version,
                            old_commit->root_id, new_commit->root_id,
                            &prepared->changes, FALSE)
        : -1;
    if (result == 0 && prepared->origin_protection) {
        VirtualProof proof = {prepared, session, new_commit->root_id, repo->version, FALSE, FALSE, 0};
        guint count = 0;
        for (GList *entry = prepared->changes; entry; entry = entry->next) {
            DiffEntry *delta = entry->data;
            if (++count > 8192 || !delta || virtual_rows(NULL, &proof, delta->name) ||
                virtual_rows(NULL, &proof, delta->new_name)) { result = -1; break; }
        }
    }
    if (old_commit)
        seaf_commit_unref(old_commit);
    if (new_commit)
        seaf_commit_unref(new_commit);
    seaf_repo_unref(repo);
    if (result < 0)
        goto fail;
    return prepared;
fail:
    cf_v2_edit_prepared_free(prepared);
    return NULL;
}

/* Exact key lookup for files, prefix lookup for removed/renamed directories.
 * This runs after Branch(master) is locked and does not scan all protected rows.
 * A failed query blocks the candidate, never silently unprotects the file. */
static gboolean
protection_found(SeafDBRow *row, void *data)
{
    (void)row;
    (void)data;
    return FALSE;
}

static char *
protection_prefix(const char *path)
{
    GString *s = g_string_new("");
    for (const char *c = path; *c; ++c) {
        if (*c == '!' || *c == '%' || *c == '_')
            g_string_append_c(s, '!');
        g_string_append_c(s, *c);
    }
    g_string_append(s, "/%");
    return g_string_free(s, FALSE);
}

static int
protection_changed_path(SeafDBTrans *trans, const char *repo_id, const char *changed)
{
    if (!changed || !*changed)
        return 0;
    char *path = changed[0] == '/' ? g_strdup(changed) : g_strconcat("/", changed, NULL);
    if (!strcmp(path, "/") || strlen(path) > 4096 || !g_utf8_validate(path, -1, NULL)) {
        g_free(path);
        return -1;
    }
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, path, -1);
    int n = seaf_db_trans_foreach_selected_row(trans,
        "SELECT 1 FROM cf_file_protection WHERE repo_id=? "
        "AND path_hash=UNHEX(?) AND path=? LIMIT 1 FOR UPDATE",
        protection_found, NULL, 3,
        "string", repo_id, "string", hash, "string", path);
    g_free(hash);
    /* 修改原因（2026-11-17）：命中与查询失败原先都返回 -1，调用方无法区分
     * "文件已定版"和"SQL 不可用"，最终只能给用户一句 authority unavailable。
     * 这里拆成 1=命中、-1=故障；判定仍然是同一批查询，不增加任何 I/O。 */
    if (n < 0) { g_free(path); return -1; }
    if (n > 0) { g_free(path); return 1; }
    char *prefix = protection_prefix(path);
    n = seaf_db_trans_foreach_selected_row(trans,
        "SELECT 1 FROM cf_file_protection FORCE INDEX (cf_file_protection_prefix) "
        "WHERE repo_id=? AND path LIKE ? ESCAPE '!' LIMIT 1 FOR UPDATE",
        protection_found, NULL, 2, "string", repo_id, "string", prefix);
    g_free(prefix);
    g_free(path);
    if (n < 0) return -1;
    return n > 0 ? 1 : 0;
}

int
cf_v2_edit_check_unscoped_branch(SeafDBTrans *trans,
                                 struct _SeafileSession *session,
                                 const char *repo_id,
                                 const char *old_commit_id,
                                 const char *new_commit_id,
                                 const CfV2EditPrepared *prepared)
{
    if (!trans || !session || !repo_id || !old_commit_id || !new_commit_id ||
        !prepared || g_strcmp0(prepared->repo_id, repo_id) ||
        g_strcmp0(prepared->old_id, old_commit_id) ||
        g_strcmp0(prepared->new_id, new_commit_id))
        return -1;
    int checkout = checkout_policy(session, trans, repo_id);
    int protection = repo_feature(session, trans, repo_id, "protection");
    if (checkout != prepared->checkout || protection != prepared->protection ||
        (prepared->origin && repo_feature(session, trans, prepared->origin, "protection") !=
         prepared->origin_protection)) return -1;
    if (prepared->origin_protection) {
        VirtualProof proof = {(CfV2EditPrepared *)prepared, session, NULL, 0, TRUE, FALSE, 0};
        for (GList *entry = prepared->changes; entry; entry = entry->next) {
            DiffEntry *delta = entry->data;
            int hit = virtual_rows(trans, &proof, delta->name);
            int renamed = virtual_rows(trans, &proof, delta->new_name);
            if (hit < 0 || renamed < 0) return -1;
            if (hit > 0 || renamed > 0) return CF_V2_EDIT_DENIED_PROTECTED;
        }
    }
    if (checkout == 0 && protection == 0) return 0;
    if (seaf_db_type(session->db) != SEAF_DB_TYPE_MYSQL)
        return -1;
    if (protection == 1) {
        guint count = 0;
        for (GList *entry = prepared->changes; entry; entry = entry->next) {
            if (++count > 8192) return -1;
            DiffEntry *delta = entry->data;
            if (!delta) return -1;
            int hit = protection_changed_path(trans, repo_id, delta->name);
            int renamed = protection_changed_path(trans, repo_id, delta->new_name);
            /* 修改原因（2026-11-17）：故障优先归为不可验证，不能被误报成
             * "文件已定版"；只有真正命中保护行才回传专用原因码。 */
            if (hit < 0 || renamed < 0) return -1;
            if (hit > 0 || renamed > 0) return CF_V2_EDIT_DENIED_PROTECTED;
        }
    }
    if (checkout == 0) return 0;
    if (guard_available(session) <= 0) return -1;

    GuardPaths guards = {g_ptr_array_new_with_free_func(g_free), FALSE};
    int n = seaf_db_trans_foreach_selected_row(trans,
        "SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) "
        "WHERE repo_id=? AND guard_id IS NOT NULL "
        "AND lease_expires_at>UTC_TIMESTAMP(6) FOR UPDATE",
        read_guard_path, &guards, 1, "string", repo_id);
    int rc = -1;
    if (n < 0 || guards.invalid) goto done;
    rc = 0;
    for (GList *entry = prepared->changes; entry && rc == 0; entry = entry->next) {
        DiffEntry *delta = entry->data;
        for (guint i = 0; i < guards.paths->len; ++i) {
            const char *path = g_ptr_array_index(guards.paths, i);
            if (path_touches(delta->name, path) || path_touches(delta->new_name, path)) {
                rc = -1;
                break;
            }
        }
    }
done:
    g_ptr_array_free(guards.paths, TRUE);
    return rc;
}
