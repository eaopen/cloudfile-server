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
/* Only a changed HEAD is retryable. Policy or SQL failure must stop
 * immediately; the caller cannot recover it by creating more commits. */
#define SEAF_BRANCH_UPDATE_ERROR         (-1)
#define SEAF_BRANCH_UPDATE_ACL_DENIED    (-2)
#define SEAF_BRANCH_UPDATE_HEAD_CONFLICT (-3)
/* 修改原因（2026-11-17）：文件定版是一种永久性拒绝，和 ACL/Guard/策略故障
 * 的处置不同（重试无意义、面向用户要说明原因）。单独编码，避免调用方把它
 * 笼统报成 "authority unavailable"。 */
#define SEAF_BRANCH_UPDATE_FILE_PROTECTED (-4)
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
