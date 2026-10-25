#ifndef CF_V2_EDIT_FENCE_H
#define CF_V2_EDIT_FENCE_H

#include <glib.h>
#include "seaf-db.h"

struct _SeafileSession;

/* 0=OFF, 1=OPTIONAL, -1=invalid configuration (fail closed).
 * Pure in-memory policy check; never queries Guard for OFF repositories. */
int cf_v2_edit_repo_checkout(struct _SeafileSession *session, const char *repo_id);

/* One candidate commit diff is calculated before acquiring Branch(master)
 * for OPTIONAL libraries. Recheck the current Guard rows under the short final
 * transaction, without traversing Seafile objects while holding Branch. */
typedef struct _CfV2EditPrepared CfV2EditPrepared;
CfV2EditPrepared *cf_v2_edit_prepare_unscoped_branch(
    struct _SeafileSession *session, const char *repo_id,
    const char *old_commit_id, const char *new_commit_id,
    const CfV2EditPrepared *prepared);
void cf_v2_edit_prepared_free(CfV2EditPrepared *prepared);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CfV2EditPrepared, cf_v2_edit_prepared_free)

/* Called only after Branch(master) FOR UPDATE, before updating Branch.
 * Zero: unscoped write does not touch a reserved file.
 * -1: guarded/conflicting/unverifiable; caller MUST rollback. */
int cf_v2_edit_check_unscoped_branch(SeafDBTrans *trans,
    struct _SeafileSession *session, const char *repo_id,
    const char *old_commit_id, const char *new_commit_id);

#endif
