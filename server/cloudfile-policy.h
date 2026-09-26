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

/* Shared read policy for an actual file/directory target (CF_FILE/CF_DIRECTORY).
 * Caller must establish native active/current subject, barriers, CE library
 * qualification and target existence on this same final transaction. This
 * adapter does not authenticate a snapshot or issue/consume a web ticket. */
int cf_policy_check_read (SeafDBTrans *trans, const char *repo, const char *path,
                         const char *provider, const char *user, json_t *snapshot,
                         int ce_permission, int kind);
#endif
