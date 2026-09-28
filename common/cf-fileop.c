/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <string.h>

#include "cf-fileop.h"
#include "cf-path.h"
#include "log.h"
#include "seafile-error.h"

/* ------------------------------------------------------------- vocabulary */

typedef struct OpSpec {
    const char *op;
    gboolean has_source;
    gboolean pathless;
    gboolean subject_is_root;
} OpSpec;

static const OpSpec op_specs[] = {
    { CF_OP_CREATE_FILE,   FALSE, FALSE, FALSE },
    { CF_OP_UPDATE_FILE,   FALSE, FALSE, FALSE },
    { CF_OP_DELETE,        FALSE, FALSE, FALSE },
    { CF_OP_MKDIR,         FALSE, FALSE, FALSE },
    { CF_OP_RENAME,        TRUE,  FALSE, FALSE },
    { CF_OP_MOVE,          TRUE,  FALSE, FALSE },
    { CF_OP_COPY,          TRUE,  FALSE, FALSE },
    { CF_OP_REVERT_FILE,   FALSE, FALSE, FALSE },
    { CF_OP_REVERT_DIR,    FALSE, FALSE, FALSE },
    { CF_OP_REVERT_REPO,   FALSE, FALSE, TRUE  },
    { CF_OP_UPDATE_DIR,    FALSE, FALSE, FALSE },
    { CF_OP_UPLOAD_BLOCKS, FALSE, TRUE,  FALSE },
    { CF_OP_SYNC_UPDATE,   FALSE, FALSE, TRUE  },
};

static const OpSpec *
find_op (const char *op)
{
    if (!op)
        return NULL;

    for (guint i = 0; i < G_N_ELEMENTS (op_specs); i++) {
        if (strcmp (op_specs[i].op, op) == 0)
            return &op_specs[i];
    }

    return NULL;
}

gboolean
cf_fileop_op_valid (const char *op)
{
    return find_op (op) != NULL;
}

gboolean
cf_fileop_op_has_source (const char *op)
{
    const OpSpec *spec = find_op (op);
    return spec ? spec->has_source : FALSE;
}

gboolean
cf_fileop_op_pathless (const char *op)
{
    const OpSpec *spec = find_op (op);
    return spec ? spec->pathless : FALSE;
}

gboolean
cf_fileop_op_subject_is_root (const char *op)
{
    const OpSpec *spec = find_op (op);
    return spec ? spec->subject_is_root : FALSE;
}

/* ---------------------------------------------------------------- context */

char *
cf_fileop_subject_path (const CfFileOp *fop)
{
    if (!fop || cf_fileop_op_pathless (fop->op))
        return NULL;

    if (cf_fileop_op_subject_is_root (fop->op))
        return g_strdup ("/");

    return cf_path_join (fop->dir, fop->name);
}

GList *
cf_fileop_subject_paths (const CfFileOp *fop)
{
    if (!fop || cf_fileop_op_pathless (fop->op))
        return NULL;

    if (!fop->names)
        return g_list_append (NULL, cf_fileop_subject_path (fop));

    GList *paths = NULL, *ptr;

    for (ptr = fop->names; ptr; ptr = ptr->next)
        paths = g_list_append (paths, cf_path_join (fop->dir, ptr->data));

    return paths;
}

GList *
cf_fileop_source_paths (const CfFileOp *fop)
{
    if (!fop || !cf_fileop_op_has_source (fop->op))
        return NULL;

    if (!fop->src_names)
        return g_list_append (NULL, cf_path_join (fop->src_dir, fop->src_name));

    GList *paths = NULL, *ptr;

    for (ptr = fop->src_names; ptr; ptr = ptr->next)
        paths = g_list_append (paths, cf_path_join (fop->src_dir, ptr->data));

    return paths;
}

/* ------------------------------------------------------------- providers */

typedef struct Provider {
    char *name;
    CfFileOpPrepareFunc prepare;
    CfFileOpCommittedFunc committed;
    CfFileOpAbortedFunc aborted;
} Provider;

static GList *providers = NULL;     /* Provider*, registration order */

void
cf_fileop_register (const char *name,
                    CfFileOpPrepareFunc prepare,
                    CfFileOpCommittedFunc committed,
                    CfFileOpAbortedFunc aborted)
{
    Provider *p = g_new0 (Provider, 1);
    p->name = g_strdup (name);
    p->prepare = prepare;
    p->committed = committed;
    p->aborted = aborted;

    providers = g_list_append (providers, p);
    seaf_message ("CloudFile: write lifecycle provider '%s' registered.\n",
                  name);
}

static void
provider_free (void *data)
{
    Provider *p = data;
    g_free (p->name);
    g_free (p);
}

void
cf_fileop_reset (void)
{
    g_list_free_full (providers, provider_free);
    providers = NULL;
}

gboolean
cf_fileop_active (void)
{
    return providers != NULL;
}

/* ------------------------------------------------------------- dispatch */

int
cf_fileop_prepare (CfFileOp *fop, GError **error)
{
    if (!providers)
        return 0;

    /* An operation string outside the vocabulary means a call site and this
     * table disagree. Refusing is the only safe answer: the alternative is to
     * let a write past every provider because nobody recognised it, which is
     * fail-open in the one place that must not be.
     */
    if (!cf_fileop_op_valid (fop->op)) {
        seaf_warning ("CloudFile: unknown file operation '%s'.\n",
                      fop->op ? fop->op : "(null)");
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Unknown file operation");
        return -1;
    }

    fop->phase = CF_FILEOP_PHASE_PREPARE;

    GList *ptr;
    for (ptr = providers; ptr; ptr = ptr->next) {
        Provider *p = ptr->data;
        if (!p->prepare)
            continue;

        if (p->prepare (fop, error) < 0) {
            /* Stop here. Later providers must not observe a write that is not
             * going to happen.
             */
            if (error && !*error)
                g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                             "Refused by CloudFile provider %s", p->name);
            return -1;
        }
    }

    return 0;
}

void
cf_fileop_committed (CfFileOp *fop)
{
    if (!providers)
        return;

    fop->phase = CF_FILEOP_PHASE_COMMITTED;

    GList *ptr;
    for (ptr = providers; ptr; ptr = ptr->next) {
        Provider *p = ptr->data;
        if (p->committed)
            p->committed (fop);
    }
}

void
cf_fileop_aborted (CfFileOp *fop)
{
    if (!providers)
        return;

    fop->phase = CF_FILEOP_PHASE_ABORTED;

    GList *ptr;
    for (ptr = providers; ptr; ptr = ptr->next) {
        Provider *p = ptr->data;
        if (p->aborted)
            p->aborted (fop);
    }
}
