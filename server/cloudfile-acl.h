/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Pure CloudFile policy core. Not an authorization entry point: callers must
 * supply current, authenticated context and the complete ancestor candidate set.
 */
#ifndef CLOUDFILE_ACL_H
#define CLOUDFILE_ACL_H
#include <stddef.h>

enum cf_subject_type { CF_GROUP = 1, CF_DEPT = 2, CF_USER = 3 };
enum cf_permission { CF_INVISIBLE = 0, CF_NONE = 1, CF_READ = 2, CF_WRITE = 3 };
enum cf_kind { CF_DIRECTORY = 0, CF_FILE = 1 };

struct cf_acl_rule {
    const char *path;
    const char *subject_id; /* provider/namespace-qualified identifier */
    int subject_type;
    int permission;
    int inherit;
    int kind;
};
struct cf_acl_context {
    const char *user_id;
    const char *const *departments;
    size_t department_count;
    const char *const *groups;
    size_t group_count;
    int ce_permission; /* only READ/WRITE establish library qualification */
    int ready;
    int active;
    int hard_readonly;
    int barrier_active;
};
struct cf_acl_result { int visible; int read; int write; };

/* 0 = evaluated; -1 = invalid input/allocation failure, result stays denied.
 * Does not implement manage delegation, persistence, caching or final commit.
 */
int cf_acl_evaluate(const struct cf_acl_context *context, const char *path,
                    int kind, const struct cf_acl_rule *rules, size_t count,
                    struct cf_acl_result *result);
#endif
