/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * JSON wire form of CfFileOp, for the Go fileserver.
 *
 * Split from cf-fileop.c so that file keeps depending on nothing but glib and
 * still compiles into the standalone test binary. jansson lives only here.
 *
 * One string argument rather than a fixed-arity searpc signature: the context
 * has thirteen optional fields and P1 adds session identity to it. Widening a
 * searpc signature means touching the registration, the client stub and every
 * caller; widening a JSON object means neither side has to move in lockstep,
 * and an older peer simply does not send the new key.
 *
 * Wire format: cloudfile-docker/docs/fileop-lifecycle.md section 4.
 */

#ifndef CF_FILEOP_JSON_H
#define CF_FILEOP_JSON_H

#include <glib.h>

#include "cf-fileop.h"

/*
 * Parse @json into a heap CfFileOp whose string fields are owned copies.
 * Returns NULL and sets @error on malformed input or an operation outside the
 * vocabulary.
 *
 * Free with cf_fileop_json_free().
 */
CfFileOp *cf_fileop_from_json (const char *json, GError **error);

void cf_fileop_json_free (CfFileOp *fop);

#endif /* CF_FILEOP_JSON_H */
