/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef CLOUDFILE_POLICY_H
#define CLOUDFILE_POLICY_H
#include <jansson.h>
#include "seaf-db.h"

/* Opt-in monotonic managed-library boundary. Zero means legacy access allowed;
 * managed, missing/drifted schema and storage failures all reject. The caller
 * holds this transaction through its final publication statement. */
gboolean cf_policy_legacy_guard_enabled (GKeyFile *config);
/* Temporary native read ticket compatibility; invalid values remain closed. */
gboolean cf_policy_allow_legacy_managed_reads (GKeyFile *config);
/* Installed extension state makes the boundary mandatory, even without an
 * opt-in setting. A missing registry cannot reopen an installed deployment. */
gboolean cf_policy_managed_guard_required (SeafDB *db, GKeyFile *config);
int cf_policy_check_legacy_library (SeafDBTrans *trans, const char *repo);
int cf_policy_check_legacy_access (SeafDB *db, GKeyFile *config, const char *repo);
/* Same decision, additionally reporting whether the refusal came from an
 * unreadable runtime (schema missing/drifted, SQL/transaction failure) rather
 * than the deliberate managed-library boundary. Callers that surface the
 * answer to a user must use this form and answer "unavailable" for the
 * former, never "no permission". @unavailable may be NULL. */
int cf_policy_check_legacy_access_reason (SeafDB *db, GKeyFile *config,
                                          const char *repo, gboolean *unavailable);
int cf_policy_enroll_managed_library (SeafDBTrans *trans, const char *repo);

/* Same final transaction, authenticated current snapshot and exact RPC target.
 * Returns 0 only for a write granted by the shared C ACL core. Not an entry API. */
int cf_policy_check_write (SeafDBTrans *trans, const char *repo, const char *path,
                           const char *provider, const char *user, json_t *snapshot,
                           int ce_permission);
/* Creation needs both parent-directory write and exact new-file policy. */
int cf_policy_check_create (SeafDBTrans *trans, const char *repo, const char *path,
                           const char *provider, const char *user, json_t *snapshot,
                           int ce_permission);

/* Ordinary conditional writes compare the canonical Seafile native username
 * against file-lock.owner_native_user, never a business/OIDC identity. Checkout
 * always requires its full editing proof. Held through Branch
 * publication; structural/legacy entry coverage remains a separate gate. */
int cf_policy_check_unleased_write (SeafDBTrans *trans, const char *repo,
                                    const char *path, const char *user);

/* Retired lease proofs are rejected. A NULL proof uses the ordinary native
 * username barrier; controlled publishing uses cf_policy_edit_publish. */
int cf_policy_check_lease_write (SeafDBTrans *trans, const char *repo, const char *path,
                                 const char *user, json_t *proof);
/* Validate the prepared checkout and optionally publish its receipt in the
 * caller's Branch transaction. Native file identity/digest are computed by
 * repo-op, never accepted from a remote request. */
int cf_policy_edit_publish (SeafDBTrans *trans, const char *repo, const char *path,
                            const char *user, json_t *conditions,
                            const char *commit_id);

/* Shared read policy for an actual file/directory target (CF_FILE/CF_DIRECTORY).
 * Caller must establish native active/current subject, barriers, native library
 * qualification state and target existence on this same final transaction.
 * ce_permission=0 is a valid nonmember; negative values are invalid. The scoped
 * core may qualify a nonmember only through applicable directory ACL/shares. This
 * adapter does not authenticate a snapshot or issue/consume a web ticket. */
int cf_policy_check_read (SeafDBTrans *trans, const char *repo, const char *path,
                         const char *provider, const char *user, json_t *snapshot,
                         int ce_permission, int kind);
/* Historical local_session proof rejects; use V2 Guard + Agent2 Journal. */
int cf_policy_check_local_session (SeafDBTrans *trans, const char *repo,
                                  const char *path, const char *object_id,
                                  int kind, json_t *root,
                                  json_t *current_subject, int ce_permission);
/* Historical optimistic receipt rejects; V2 Go owns Branch CAS. */
int cf_policy_optimistic_receipt (SeafDBTrans *trans, const char *repo,
                                  json_t *conditions, const char *commit_id);

int cf_policy_check_device_commit (SeafDBTrans *trans, const char *repo,
                                  const char *path, json_t *conditions,
                                  json_t *subject, int qualification);

#endif
