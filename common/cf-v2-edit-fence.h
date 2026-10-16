#ifndef CF_V2_EDIT_FENCE_H
#define CF_V2_EDIT_FENCE_H

#include <glib.h>
#include "seaf-db.h"

struct _SeafileSession;

/* Called only after Branch(master) FOR UPDATE, before updating Branch.
 * Zero: unscoped write does not touch a reserved file.
 * -1: guarded/conflicting/unverifiable; caller MUST rollback. */
int cf_v2_edit_check_unscoped_branch(SeafDBTrans *trans,
    struct _SeafileSession *session, const char *repo_id,
    const char *old_commit_id, const char *new_commit_id);

#endif
