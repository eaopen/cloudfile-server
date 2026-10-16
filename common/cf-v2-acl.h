#ifndef CF_V2_ACL_H
#define CF_V2_ACL_H
#include <glib.h>
#include "seaf-db.h"
struct _SeafileSession;
/* Zero only for an unprotected native repo. Errors and protected scopes deny
 * unscoped C/sync paths. This is ACL, not the CF-EDIT writer reservation. */
/* Until two-repository/source proof is wired, transfer operations are closed
 * while ACL mode is enabled, including copies started before a rule appears. */
int cf_v2_acl_transfer(struct _SeafileSession *session);
int cf_v2_acl_unscoped(struct _SeafileSession *session, const char *repo_id);
int cf_v2_acl_final(SeafDBTrans *trans, struct _SeafileSession *session,
                    const char *repo_id);
int cf_v2_acl_final_scoped(SeafDBTrans *trans, struct _SeafileSession *session,
    const char *repo_id, const char *old_id, const char *new_id, const char *actor);
#endif
