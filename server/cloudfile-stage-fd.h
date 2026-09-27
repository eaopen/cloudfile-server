#ifndef CLOUDFILE_STAGE_FD_H
#define CLOUDFILE_STAGE_FD_H
#include <glib.h>

/* Authenticated Unix peer packet receiver only, not target authorization.
 * Caller owns both returned FD and g_free(metadata). No listener is installed. */
int cf_stage_receive_fd (int connection, guint32 expected_peer_uid,
                         int *stage_fd, char **metadata);
#endif
