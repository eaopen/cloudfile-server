/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef STORAGE_BACKEND_MULTI_H
#define STORAGE_BACKEND_MULTI_H

#include <glib.h>

#include "obj-backend.h"
#include "block-backend.h"
#include "seaf-db.h"

ObjBackend *
obj_backend_multi_new (GKeyFile *config,
                       SeafDB *db,
                       const char *seaf_dir,
                       const char *obj_type);

BlockBackend *
block_backend_multi_new (GKeyFile *config,
                         SeafDB *db,
                         const char *seaf_dir,
                         const char *tmp_dir);

#endif /* STORAGE_BACKEND_MULTI_H */
