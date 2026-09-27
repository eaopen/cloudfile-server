/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef CLOUDFILE_POLICY_H
#define CLOUDFILE_POLICY_H
#include <jansson.h>
#include "seaf-db.h"

/* Same final transaction, authenticated current snapshot and exact RPC target.
 * Returns 0 only for a write granted by the shared C ACL core. Not an entry API. */
int cf_policy_check_write (SeafDBTrans *trans, const char *repo, const char *path,
                           const char *provider, const char *user, json_t *snapshot,
                           int ce_permission);
/* Creation needs both parent-directory write and exact new-file policy. */
int cf_policy_check_create (SeafDBTrans *trans, const char *repo, const char *path,
                           const char *provider, const char *user, json_t *snapshot,
                           int ce_permission);

/* Ordinary conditional writes carry no lease proof: an active file lease must
 * reject them, including writes by the owner. Held through Branch publication.
 * Exclusive-edit proof and structural/legacy entry coverage are separate gates. */
int cf_policy_check_unleased_write (SeafDBTrans *trans, const char *repo, const char *path);

/* Trusted internal lease proof, not an HTTP identity. Caller also checks the
 * actual current native file equals proof.base_version in this transaction. */
int cf_policy_check_lease_write (SeafDBTrans *trans, const char *repo, const char *path,
                                 const char *user, json_t *proof);

/* Shared read policy for an actual file/directory target (CF_FILE/CF_DIRECTORY).
 * Caller must establish native active/current subject, barriers, CE library
 * qualification and target existence on this same final transaction. This
 * adapter does not authenticate a snapshot or issue/consume a web ticket. */
int cf_policy_check_read (SeafDBTrans *trans, const char *repo, const char *path,
                         const char *provider, const char *user, json_t *snapshot,
                         int ce_permission, int kind);
/* Optional internal local_session condition. Never replaces current CE/C,
 * subject, native object or OIDC checks. Locks persist through ticket use. */
int cf_policy_check_local_session (SeafDBTrans *trans, const char *repo,
                                  const char *path, const char *object_id,
                                  int kind, json_t *root,
                                  json_t *current_subject, int ce_permission);
#endif
