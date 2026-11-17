#ifndef CF_V2_EDIT_FENCE_H
#define CF_V2_EDIT_FENCE_H

#include <glib.h>
#include "seaf-db.h"

struct _SeafileSession;

/* 0=OFF, 1=OPTIONAL, -1=invalid configuration (fail closed).
 * Pure in-memory policy check; never queries Guard for OFF repositories. */
int cf_v2_edit_repo_checkout(struct _SeafileSession *session, const char *repo_id);

// Separate permanent file-protection scope; never interpreted as a Checkout.
int cf_v2_file_protection_policy(struct _SeafileSession *session, const char *repo_id);

/* One candidate commit diff is calculated before acquiring Branch(master)
 * for OPTIONAL libraries. Recheck the current Guard rows under the short final
 * transaction, without traversing Seafile objects while holding Branch. */
typedef struct _CfV2EditPrepared CfV2EditPrepared;
CfV2EditPrepared *cf_v2_edit_prepare_unscoped_branch(
    struct _SeafileSession *session, const char *repo_id,
    const char *old_commit_id, const char *new_commit_id);
void cf_v2_edit_prepared_free(CfV2EditPrepared *prepared);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CfV2EditPrepared, cf_v2_edit_prepared_free)

/* Lock origin before the virtual Branch; shared with Hub protect/unprotect. */
int cf_v2_edit_lock_origin(SeafDBTrans *trans, const CfV2EditPrepared *prepared);

/* Called only after Branch(master) FOR UPDATE, before updating Branch.
 * Zero: unscoped write does not touch a reserved file.
 * CF_V2_EDIT_DENIED_PROTECTED: the candidate diff touches a protected original
 *   or one of its ancestor directories. Distinguished from other denials so the
 *   caller can report "file is locked" instead of a generic authority failure.
 * Any other negative value: guarded/conflicting/unverifiable; caller MUST rollback. */
/* 取值刻意远离 -1/-2/-3，避免与 SEAF_BRANCH_UPDATE_* 的数值巧合；
 * 两套常量在同一处调用点相邻出现，数值重叠会造成误读。 */
#define CF_V2_EDIT_DENIED_PROTECTED (-100)

int cf_v2_edit_check_unscoped_branch(SeafDBTrans *trans,
    struct _SeafileSession *session, const char *repo_id,
    const char *old_commit_id, const char *new_commit_id,
    const CfV2EditPrepared *prepared);

#endif
