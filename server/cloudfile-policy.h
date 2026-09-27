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
/* Private final intent check only. Does NOT publish or finish an intent.
 * Caller must derive both object IDs from real old/candidate commit trees and
 * hold actual current identity/Branch barriers. Until atomic receipt/events are
 * installed, ordinary Branch publication continues to reject local_commit. */
int cf_policy_check_local_commit (SeafDBTrans *trans, const char *repo,
                                 const char *path, const char *old_object_id,
                                 const char *new_object_id, json_t *root,
                                 json_t *current_subject, int ce_permission);
int cf_policy_check_local_index (SeafDBTrans *trans, const char *repo,
                                const char *path, const char *old_object_id,
                                const char *measured_sha256, gint64 measured_bytes,
                                json_t *root, json_t *current_subject, int ce_permission);
/* Caller owns the actual authorized staging/index transaction. Arguments must
 * come from measured FD + native index output, never a public JSON receipt.
 * No Branch, completed state or publication fact is written here. */
int cf_local_commit_record_index (SeafDBTrans *trans, const char *provider,
                                  const char *user, const char *commit_id,
                                  const char *store_id, const char *expected_revision,
                                  const char *measured_sha256, gint64 measured_bytes,
                                  const char *indexed_file_id, char **receipt_revision);
/* Private post-Branch completion, on the SAME transaction. Requires the
 * exact commit_id mutation fact in both outbox and audit already appended on
 * that transaction. Missing facts/guards roll back the caller's publication. */
int cf_local_commit_finish (SeafDBTrans *trans, const char *repo, const char *path,
                           const char *old_file_id, const char *new_file_id,
                           const char *published_head, json_t *root,
                           json_t *current_subject, int ce_permission);
int cf_local_commit_append_fact (SeafDBTrans *trans, const char *repo, const char *path,
                                const char *old_file_id, const char *new_file_id,
                                json_t *root, json_t *current_subject, int ce_permission);
#endif
