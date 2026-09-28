/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * CloudFile directory ACL: the policy, with no I/O.
 *
 * Deliberately split from cf-acl.c so that this half depends on nothing but
 * glib and can be compiled and tested on its own against the shared case set
 * in cloudfile-docker/docs/acl-cases.json -- the same cases the Python and Go
 * implementations run. cf-acl.c supplies the config, database and group
 * lookups and calls in here to decide.
 *
 * Semantics: cloudfile-docker/docs/acl-semantics.md
 */

#ifndef CF_ACL_RESOLVE_H
#define CF_ACL_RESOLVE_H

#include <glib.h>

/* Rule permissions, strictest first. See acl-semantics.md section 2. */
#define CF_PERM_INVISIBLE   0
#define CF_PERM_NONE        1
#define CF_PERM_R           2
#define CF_PERM_RW          3
#define CF_PERM_UNKNOWN    -1

/* Subject types: more specific wins outright at the same level. */
#define CF_SUBJ_UNKNOWN     0
#define CF_SUBJ_GROUP       1
#define CF_SUBJ_DEPT        2
#define CF_SUBJ_USER        3

typedef struct CfAclRule {
    char *path;             /* normalized */
    int   subject_type;
    char *subject;
    int   permission;
    int   inherit;
} CfAclRule;

CfAclRule *cf_acl_rule_new (const char *path,
                            int subject_type,
                            const char *subject,
                            int permission,
                            int inherit);

void cf_acl_rule_free (CfAclRule *rule);

int cf_acl_perm_to_level (const char *perm);
int cf_acl_subject_type_to_level (const char *subject_type);

/* Key used in the subject hash set: "<type-level>:<subject>". */
char *cf_acl_subject_key (int subject_type, const char *subject);

char *cf_acl_normalize_path (const char *path);

/* Every level from the root down to @path, inclusive. Free with g_free. */
GList *cf_acl_ancestors (const char *path);

/*
 * Narrow @native_perm for @path given @rules and the caller's @subjects.
 *
 * @subjects is a GHashTable used as a set of cf_acl_subject_key() strings.
 * Returns a newly allocated permission string, or NULL for no access.
 */
char *cf_acl_resolve (GList *rules,
                      GHashTable *subjects,
                      const char *path,
                      const char *native_perm);

/*
 * Find the first path at or below @root that @subjects cannot access at all.
 *
 * Used to answer "may this repo be synced" and "may this folder be
 * zip-downloaded": both operations hand the whole subtree to the client in one
 * go, so a single unreadable descendant has to block them.
 *
 * Only rule paths can change the outcome, so scanning the rules plus @root
 * itself is sufficient -- there is no need to walk the actual directory tree.
 *
 * Returns a newly allocated path, or NULL when the whole subtree is
 * accessible.
 */
char *cf_acl_find_restricted (GList *rules,
                              GHashTable *subjects,
                              const char *root,
                              const char *native_perm);

#endif /* CF_ACL_RESOLVE_H */
