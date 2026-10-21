#ifndef CF_ACL_H
#define CF_ACL_H
#include <glib.h>

/* CloudFile V2 native permission adapter. One Hub Authority evaluates
 * authenticated user + actual directory path; shared roots are independent
 * qualifications. A sparse ACL is not a repository classification.
 * Missing permission context or a failed Authority must not widen access. */
void cf_acl_init (void);
gboolean cf_acl_enabled (void);
char *cf_acl_apply (const char *repo_id, const char *path,
                    const char *user, const char *native_perm,
                    gboolean *unavailable);
GList *cf_acl_filter_dirents (const char *repo_id, const char *dir_path,
                             const char *user, GList *dirents);
char *cf_acl_find_restricted_path (const char *repo_id, const char *path,
                                   const char *user, const char *native_perm);
#endif /* CF_ACL_H */
