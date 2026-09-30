/* Bounded transport only: the callback remains the complete scalar engine. */
#ifndef CF_PERMISSION_MANY_H
#define CF_PERMISSION_MANY_H
#include <glib.h>

typedef char *(*CfPermissionScalar)(const char *, const char *, const char *, GError **);
char *cf_permissions_many_json(const char *request_json, CfPermissionScalar scalar,
                               GError **error);
#endif
