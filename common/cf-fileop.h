/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * CloudFile write lifecycle extension point.
 *
 * The read-side seams in cf-ext.h answer "may this user see it". This one
 * answers the two questions the write side needs and CE has nowhere to ask:
 *
 *   PREPARE    -- may this write happen? A provider may refuse, and nothing
 *                 has been persisted yet when it does.
 *   COMMITTED  -- it happened. An immutable fact, exactly once per successful
 *                 operation.
 *   ABORTED    -- it did not happen after PREPARE allowed it. Best effort.
 *
 * Six capabilities were blocked on this: file lock, checkout, OnlyOffice
 * write-back, file properties, tags, and metadata following a rename. Building
 * the lock first would have produced two of these -- the lock's veto point and
 * the file_op event source -- covering the same set of write entry points. So
 * the contract comes first and the lock registers into it.
 *
 * Everything here depends on nothing but glib, so the vocabulary, the path
 * rules and the dispatcher all compile into tests/cf-fileop without the
 * seafile build. Providers supply their own I/O.
 *
 * Spec: cloudfile-docker/docs/fileop-lifecycle.md
 * Cases: cloudfile-docker/docs/fileop-cases.json
 */

#ifndef CF_FILEOP_H
#define CF_FILEOP_H

#include <glib.h>

/* ------------------------------------------------------------- vocabulary */

/*
 * Stable strings, not an enum: these cross C, Go, searpc and eventually
 * Python. An ordinal that shifts in one place is a silent semantic error;
 * a string that shifts fails the vocabulary check loudly.
 */
#define CF_OP_CREATE_FILE   "create-file"
#define CF_OP_UPDATE_FILE   "update-file"
#define CF_OP_DELETE        "delete"
#define CF_OP_MKDIR         "mkdir"
#define CF_OP_RENAME        "rename"
#define CF_OP_MOVE          "move"
#define CF_OP_COPY          "copy"
#define CF_OP_REVERT_FILE   "revert-file"
#define CF_OP_REVERT_DIR    "revert-dir"
#define CF_OP_REVERT_REPO   "revert-repo"
#define CF_OP_UPDATE_DIR    "update-dir"
#define CF_OP_UPLOAD_BLOCKS "upload-blocks"
#define CF_OP_SYNC_UPDATE   "sync-update"

/* Whether @op is in the vocabulary above. */
gboolean cf_fileop_op_valid (const char *op);

/* Whether @op carries src_repo_id / src_path. */
gboolean cf_fileop_op_has_source (const char *op);

/*
 * Whether @op has no object path at all. Only upload-blocks: it writes blocks
 * into the object store without touching any directory tree.
 *
 * A lock provider MUST ignore these. With no path there is no lock subject, so
 * refusing here refuses at random; the real adjudication happens in the
 * create-file or update-file that follows.
 */
gboolean cf_fileop_op_pathless (const char *op);

/* Whether @op's subject is the whole library rather than one path. */
gboolean cf_fileop_op_subject_is_root (const char *op);

/* ----------------------------------------------------------- error codes */

/*
 * CloudFile's own codes, set into SEAFILE_DOMAIN alongside the upstream ones.
 *
 * Deliberately based at 600 rather than added to include/seafile-error.h:
 * that is an upstream file we have not had to patch, and the fork cost of
 * touching one more upstream file is paid at every sync, forever. Upstream is
 * at 522, so 600 leaves it room to grow into.
 */
#define CF_ERR_FILE_LOCKED      600     /* -> 423 */
#define CF_ERR_VERSION_MISMATCH 601     /* -> 409 */

/* ---------------------------------------------------------------- context */

typedef enum {
    CF_FILEOP_PHASE_PREPARE = 0,
    CF_FILEOP_PHASE_COMMITTED,
    CF_FILEOP_PHASE_ABORTED,
} CfFileOpPhase;

/*
 * What a provider is told about a write. Read-only: a provider may refuse,
 * never rewrite. If providers could rewrite, their registration order would
 * decide the result, and nobody designed that order.
 *
 * Call sites fill the raw pieces; the dispatcher joins and normalizes. That
 * ordering is what keeps an inactive build free: nothing is allocated until
 * after the active check.
 */
typedef struct CfFileOp {
    CfFileOpPhase phase;        /* set by the dispatcher */
    const char *op;

    const char *repo_id;
    const char *dir;            /* parent dir, or the object itself */
    const char *name;           /* entry under @dir; NULL when @dir is it */
    GList      *names;          /* batch: char* entry names under @dir */

    /*
     * A move writes both ends: the entry leaves the source directory. So the
     * source is not merely informational for move and rename -- a lock on the
     * source must refuse it. Batch moves need the list for the same reason
     * the destination does.
     */
    const char *src_repo_id;
    const char *src_dir;
    const char *src_name;
    GList      *src_names;

    const char *user;           /* Seafile identity, NOT an email address */
    const char *client;         /* session identity; NULL until P1 */
    const char *expect_commit_id;

    /* COMMITTED only. */
    const char *commit_id;
    const char *file_id;
} CfFileOp;

/*
 * The normalized object path of @fop: cf_path_join(dir, name).
 * Returns a newly allocated string; caller frees. NULL for pathless ops.
 */
char *cf_fileop_subject_path (const CfFileOp *fop);

/*
 * Every normalized object path of @fop. One element for a single-object
 * operation, one per entry for a batch. Free with
 * g_list_free_full(list, g_free). NULL for pathless ops.
 */
GList *cf_fileop_subject_paths (const CfFileOp *fop);

/*
 * Every normalized source path of @fop, for the ops that have one. NULL when
 * @op carries no source. Free with g_list_free_full(list, g_free).
 */
GList *cf_fileop_source_paths (const CfFileOp *fop);

/* ------------------------------------------------------------- providers */

/*
 * Refuse by returning -1 and setting @error; the code reaches the client, so
 * the message must explain why (who holds the lock, how long is left) rather
 * than just "permission denied".
 *
 * @fop is borrowed and must not be modified or retained.
 */
typedef int (*CfFileOpPrepareFunc) (const CfFileOp *fop, GError **error);

/*
 * The write happened. Cannot fail the operation -- the file has already
 * changed, and returning an error here would make the client retry something
 * that already took effect. Log and move on.
 */
typedef void (*CfFileOpCommittedFunc) (const CfFileOp *fop);

/*
 * PREPARE allowed it and it did not happen. Best effort: no ABORTED arrives
 * after a crash. A provider needing reserve/release semantics must carry its
 * own lease and timeout instead of relying on this -- which is exactly why
 * the file lock is built on heartbeats and lease_until.
 */
typedef void (*CfFileOpAbortedFunc) (const CfFileOp *fop);

/* Register a capability. @name is for logging. Any function may be NULL. */
void cf_fileop_register (const char *name,
                         CfFileOpPrepareFunc prepare,
                         CfFileOpCommittedFunc committed,
                         CfFileOpAbortedFunc aborted);

/* Drop every provider. Tests only; the server never unregisters. */
void cf_fileop_reset (void);

/*
 * Whether any provider is registered.
 *
 * Every call site is guarded on this, so a build with no capability does one
 * global read and returns: no context is built, no path is normalized, no
 * query is made, and no return code changes.
 */
gboolean cf_fileop_active (void);

/* ------------------------------------------------------------- dispatch */

/*
 * Run @fop past every provider in registration order and stop at the first
 * refusal. Returns 0 to allow, -1 to refuse with @error set.
 *
 * Providers after a refusal do not run: the write is not going to happen, so
 * letting them observe it would make a refused operation have side effects.
 */
int cf_fileop_prepare (CfFileOp *fop, GError **error);

void cf_fileop_committed (CfFileOp *fop);
void cf_fileop_aborted (CfFileOp *fop);

/*
 * Call-site sugar. The guard lives inside the macro so the compound literal is
 * never constructed on an inactive build, and so a call site stays one
 * statement instead of five.
 *
 *   if (CF_FILEOP_PREPARE (CF_OP_CREATE_FILE, error,
 *                          .repo_id = repo_id, .dir = canon_path,
 *                          .name = file_name, .user = user) < 0) {
 *       ret = -1;
 *       goto out;
 *   }
 */
#define CF_FILEOP_PREPARE(_op, _error, ...)                                 \
    (cf_fileop_active ()                                                    \
     ? cf_fileop_prepare (&(CfFileOp){ .op = (_op), __VA_ARGS__ }, (_error))\
     : 0)

#define CF_FILEOP_COMMITTED(_op, ...)                                       \
    do {                                                                    \
        if (cf_fileop_active ())                                            \
            cf_fileop_committed (&(CfFileOp){ .op = (_op), __VA_ARGS__ });  \
    } while (0)

#define CF_FILEOP_ABORTED(_op, ...)                                         \
    do {                                                                    \
        if (cf_fileop_active ())                                            \
            cf_fileop_aborted (&(CfFileOp){ .op = (_op), __VA_ARGS__ });    \
    } while (0)

#endif /* CF_FILEOP_H */
