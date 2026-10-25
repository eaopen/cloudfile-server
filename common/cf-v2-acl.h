#ifndef CF_V2_ACL_H
#define CF_V2_ACL_H
#include <glib.h>
#include "seaf-db.h"
struct _SeafileSession;
/* Only paths without an authenticated actor/target must prove their complete
 * repository scope. This is an operation constraint, not a repository class.
 * Path-qualified actions use V2 Authority instead. */
/* Guard owns a dedicated SQL connection and both library advisory locks.
 * Protected/virtual scopes or any proof failure return NULL. Callers must run
 * synchronously while the guard lives; cleanup releases connection locks. */
typedef struct CfV2ACLTransfer CfV2ACLTransfer;
CfV2ACLTransfer *cf_v2_acl_transfer_begin(struct _SeafileSession *session,
    const char *source, const char *destination);
void cf_v2_acl_transfer_free(CfV2ACLTransfer *guard);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CfV2ACLTransfer, cf_v2_acl_transfer_free)
/* Public-link token is checked by Seahub; this rechecks only the link path.
 * 0=authorized target, 1=denied/unsupported target, -1=policy unavailable.
 * Missing SQL/DB must never be presented as a user's permission denial. */
int cf_v2_acl_public_link_scope(struct _SeafileSession *session,
                                const char *repo, const char *path,
                                gboolean subtree);
/* Optimistic ACL proof for the native two-phase Branch publication:
 * read a bounded, ordered hash of ALL ACL row keys+revisions for the library
 * and its optional CE virtual origin. No HTTP, no permission decision here.
 * With trans!=NULL, use current locking reads AFTER Branch(master) is locked.
 * 0=success; negative=unavailable. proof[65] is a SHA-256 hex digest. */
int cf_v2_acl_proof(SeafDBTrans *trans, struct _SeafileSession *session,
                    const char *repo_id, char proof[65], gboolean *protected);

/* Expensive diff and current Hub write authorization run before acquiring
 * Branch(master). The final short transaction only validates the proof. */
int cf_v2_acl_preflight_scoped(struct _SeafileSession *session,
                              const char *repo_id, const char *old_id,
                              const char *new_id, const char *actor);
int cf_v2_acl_unscoped(struct _SeafileSession *session, const char *repo_id);
int cf_v2_acl_final(SeafDBTrans *trans, struct _SeafileSession *session,
                    const char *repo_id);

/* Transport only: one internal-JWT POST to /cloudfile/acl/check/. Returns the
 * HTTP status, or -1 when the channel is unusable. @body is reset and holds the
 * reply on 200. The single implementation of that handshake; cf-v2-share.c
 * consumes it rather than keeping a second copy. */
long cf_v2_acl_check_json(struct _SeafileSession *session, const char *payload,
                          GString *body);
#endif
