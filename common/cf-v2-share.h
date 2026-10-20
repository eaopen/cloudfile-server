/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef CF_V2_SHARE_H
#define CF_V2_SHARE_H

#include <glib.h>

/*
 * Independent directory-share qualification for the native permission RPCs.
 *
 * A CloudFile V2 directory share exists for accounts with no library
 * membership at all. Native Seafile has no such concept: repo-perm.c only knows
 * owner, user share, group share and public, so the library gate answers "no
 * permission" and every write entry (upload link, mkdir, rename, delete,
 * seafdav) stops there. Reads worked because they were re-adjudicated above the
 * gate; writes were not, which is why a share-only account could browse and
 * download but never upload.
 *
 * This capability answers that one question and nothing else: for an exact path
 * under an already-published share root, it reports the V2 permission the Hub
 * authority computes. It cannot grant anything outside a qualified root, it
 * never returns "admin", and it is consulted only after the native gate denied.
 */

/* Auto-register for the three-table V2 schema and an internal Hub channel.
 * No legacy managed-library schema or additional feature switch is required. */
void cf_v2_share_init (void);

/* "r" or "rw" for a share-qualified path, else NULL. Never "admin". */
char *cf_v2_share_grant (const char *repo_id, const char *path, const char *user,
                         gboolean *unavailable);

#endif /* CF_V2_SHARE_H */
