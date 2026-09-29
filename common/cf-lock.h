/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* CloudFile's Hub-owned UID lease write barrier. */

#ifndef CF_LOCK_H
#define CF_LOCK_H

#include <glib.h>

/* Registers the write-lifecycle provider when [cloudfile] file_lock_enabled
 * is true and lock_backend is either unset or "cloudfile". */
void cf_lock_init (void);

gboolean cf_lock_enabled (void);

/* Narrow synchronous exception for the native edit publisher. The final
 * Branch transaction must validate and complete the checkout proof. */
void cf_lock_controlled_publish_enter (const char *repo, const char *path);
void cf_lock_controlled_publish_leave (void);

#endif /* CF_LOCK_H */
