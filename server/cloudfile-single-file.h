#ifndef CLOUDFILE_SINGLE_FILE_H
#define CLOUDFILE_SINGLE_FILE_H
#include "fs-mgr.h"
#include "commit-mgr.h"

/* Immutable native trees only. Not a head CAS or authorization operation.
 * Derives object IDs from the actual trees, not a caller's claimed file IDs. */
int cf_check_single_file_candidate (SeafFSManager *fs, const char *store_id,
                                    SeafCommit *base, SeafCommit *candidate,
                                    const char *path, const char *native_username,
                                    char old_file[41], char new_file[41]);
/* Builds/saves only immutable candidate objects, never publishes a Branch.
 * Caller must supply actual current base and indexed intent target under its
 * owned authorization; returns an owned commit reference or NULL. */
SeafCommit *cf_build_single_file_candidate (SeafFSManager *fs, SeafCommitManager *commits,
                                           SeafCommit *base, const char *path,
                                           const char *expected_old_file,
                                           const char *indexed_new_file,
                                           const char *native_username);
#endif
