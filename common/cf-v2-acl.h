#ifndef CF_V2_ACL_H
#define CF_V2_ACL_H
#include <glib.h>
#include "seaf-db.h"
struct _SeafileSession;
/* Zero only for an unprotected native repo. Errors and protected scopes deny
 * unscoped C/sync paths. This is ACL, not the CF-EDIT writer reservation. */
/* Guard owns a dedicated SQL connection and both library advisory locks.
 * Protected/virtual scopes or any proof failure return NULL. Callers must run
 * synchronously while the guard lives; cleanup releases connection locks. */
typedef struct CfV2ACLTransfer CfV2ACLTransfer;
CfV2ACLTransfer *cf_v2_acl_transfer_begin(struct _SeafileSession *session,
    const char *source, const char *destination);
void cf_v2_acl_transfer_free(CfV2ACLTransfer *guard);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CfV2ACLTransfer, cf_v2_acl_transfer_free)
int cf_v2_acl_unscoped(struct _SeafileSession *session, const char *repo_id);
int cf_v2_acl_final(SeafDBTrans *trans, struct _SeafileSession *session,
                    const char *repo_id);
int cf_v2_acl_final_scoped(SeafDBTrans *trans, struct _SeafileSession *session,
    const char *repo_id, const char *old_id, const char *new_id, const char *actor);
#endif
