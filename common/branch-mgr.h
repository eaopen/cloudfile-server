#ifndef SEAF_BRANCH_MGR_H
#define SEAF_BRANCH_MGR_H

#include "commit-mgr.h"
#define NO_BRANCH "-"

typedef struct _SeafBranch SeafBranch;

struct _SeafBranch {
    int   ref;
    char *name;
    char  repo_id[37];
    char  commit_id[41];
};

SeafBranch *seaf_branch_new (const char *name,
                             const char *repo_id,
                             const char *commit_id);
void seaf_branch_free (SeafBranch *branch);
void seaf_branch_set_commit (SeafBranch *branch, const char *commit_id);

void seaf_branch_ref (SeafBranch *branch);
void seaf_branch_unref (SeafBranch *branch);


typedef struct _SeafBranchManager SeafBranchManager;
typedef struct _SeafBranchManagerPriv SeafBranchManagerPriv;

struct _SeafileSession;
struct _SeafBranchManager {
    struct _SeafileSession *seaf;

    SeafBranchManagerPriv *priv;
};

SeafBranchManager *seaf_branch_manager_new (struct _SeafileSession *seaf);
int seaf_branch_manager_init (SeafBranchManager *mgr);

int
seaf_branch_manager_add_branch (SeafBranchManager *mgr, SeafBranch *branch);

int
seaf_branch_manager_del_branch (SeafBranchManager *mgr,
                                const char *repo_id,
                                const char *name);

void
seaf_branch_list_free (GList *blist);

int
seaf_branch_manager_update_branch (SeafBranchManager *mgr,
                                   SeafBranch *branch);

#ifdef SEAFILE_SERVER
#ifdef FULL_FEATURE
#include "seaf-db.h"
/* Internal final read guard. The caller owns this transaction and must verify
 * target kind/existence/content version before releasing data or a ticket.
 * No commit/close or public authentication is performed here. All acquired
 * authority locks remain attached to trans until its owner closes it. */
int seaf_branch_manager_check_read_with_barriers (SeafBranchManager *mgr,
    SeafDBTrans *trans, const char *repo_id, const char *path, int kind,
    const char *conditions, const char *native_username);
/* Additionally pins master head and resolves exact object/kind by path using
 * that immutable commit. The owner must consume inside trans, not afterward. */
int seaf_branch_manager_check_read_target (SeafBranchManager *mgr, SeafDBTrans *trans,
    const char *repo_id, const char *path, int kind, const char *head_id,
    const char *object_id, const char *conditions, const char *native_username);
#endif
/**
 * Atomically test whether the current head commit id on @branch
 * is the same as @old_commit_id and update branch in db.
 */
int
seaf_branch_manager_test_and_update_branch (SeafBranchManager *mgr,
                                            SeafBranch *branch,
                                            const char *old_commit_id,
                                            gboolean check_gc,
                                            const char *last_gc_id,
                                            const char *origin_repo_id,
                                            gboolean *gc_conflict);

/* Privileged internal primitive. Scopes are trusted by the caller; this
 * serializes durable barriers and rechecks the native account and exact
 * Profile business binding, but is not an OIDC/context/ACL/lifecycle/lock proof.
 * Returns -2 for invalid/unavailable/fenced scope coordination. */
int
seaf_branch_manager_test_and_update_branch_with_barriers (
    SeafBranchManager *mgr, SeafBranch *branch, const char *old_commit_id,
    gboolean check_gc, const char *last_gc_id, const char *origin_repo_id,
    gboolean *gc_conflict, const char *scopes_json, const char *native_username);
#endif

SeafBranch *
seaf_branch_manager_get_branch (SeafBranchManager *mgr,
                                const char *repo_id,
                                const char *name);


gboolean
seaf_branch_manager_branch_exists (SeafBranchManager *mgr,
                                   const char *repo_id,
                                   const char *name);

GList *
seaf_branch_manager_get_branch_list (SeafBranchManager *mgr,
                                     const char *repo_id);

gint64
seaf_branch_manager_calculate_branch_size (SeafBranchManager *mgr,
                                           const char *repo_id, 
                                           const char *commit_id);
#endif /* SEAF_BRANCH_MGR_H */
