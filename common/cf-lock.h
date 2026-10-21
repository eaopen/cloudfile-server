/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* CloudFile's path-keyed V2 Checkout write barrier. */

#ifndef CF_LOCK_H
#define CF_LOCK_H

#include <glib.h>

/* Registers the write-lifecycle provider when [cloudfile] checkout_enabled
 * is true and lock_backend is either unset or "cloudfile". */
void cf_lock_init (void);

gboolean cf_lock_enabled (void);

#endif /* CF_LOCK_H */
