/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * CloudFile path normalization -- baseline, no I/O.
 *
 * This lived in cf-acl-resolve.c until the write lifecycle seam needed it
 * too. Path normalization is a framework-level fact, not the directory ACL's:
 * ACL keys rules by path, the lock keys leases by path, and the two MUST agree
 * byte for byte or a lock on /a/b/ and a rule on /a/b are about different
 * objects. Leaving it inside a capability would also mean the baseline seam
 * imports a capability at runtime, and that the ACL switch decides whether
 * paths can be normalized at all.
 *
 * Same move, same reasoning as acl/subjects.py -> cloudfile_ext/identity.py on
 * the Hub side (FEATURES.md item 79). cf_acl_normalize_path() stays as a thin
 * forwarder so the ACL's own tests did not have to change.
 *
 * Depends on nothing but glib, so it compiles into the standalone test
 * binaries alongside the pure-policy halves of the capabilities.
 *
 * Rules: cloudfile-docker/docs/fileop-lifecycle.md section 4.
 */

#ifndef CF_PATH_H
#define CF_PATH_H

#include <glib.h>

/*
 * Collapse separators, force a leading slash, strip the trailing one. The
 * empty path and NULL both normalize to "/".
 *
 * Deliberately leaves case and Unicode composition alone: Seafile paths are
 * byte-sensitive, and folding them here would let two distinct directories
 * share one entry.
 *
 * Returns a newly allocated string; caller frees.
 */
char *cf_path_normalize (const char *path);

/*
 * Normalized @dir with @entry appended. @entry may itself carry separators or
 * a leading slash; it is joined and then normalized as a whole, so
 * ("/a", "/b/c") and ("/a/", "b//c") both give "/a/b/c".
 *
 * A NULL or empty @entry means @dir is itself the object -- that is the mkdir,
 * revert-dir and update-dir shape, where there is no entry name to append.
 *
 * Returns a newly allocated string; caller frees.
 */
char *cf_path_join (const char *dir, const char *entry);

/*
 * Whether @path has @component as one of its slash-separated components.
 *
 * Component-wise, deliberately not a substring or a prefix match:
 *
 *   - A substring makes "notes-secret.txt" match a component of "secret", so a
 *     rule meant for one object quietly covers others.
 *   - A prefix cannot express "anywhere below", and it cannot be seeded by a
 *     test that has to create the marked directory first.
 *
 * Empty or NULL @component never matches -- that is how "no marker configured"
 * is spelled, and it must not degenerate into "matches everything".
 */
gboolean cf_path_has_component (const char *path, const char *component);

#endif /* CF_PATH_H */
