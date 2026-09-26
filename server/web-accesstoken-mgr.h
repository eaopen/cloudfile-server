/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef WEB_ACCESSTOKEN_MGR_H
#define WEB_ACCESSTOKEN_MGR_H

struct _SeafileSession;

struct WebATPriv;

struct _SeafWebAccessTokenManager {
    struct _SeafileSession	*seaf;
    struct WebATPriv *priv;
};
typedef struct _SeafWebAccessTokenManager SeafWebAccessTokenManager;

SeafWebAccessTokenManager* seaf_web_at_manager_new (struct _SeafileSession *seaf);

int
seaf_web_at_manager_start (SeafWebAccessTokenManager *mgr);

/*
 * Returns an access token for the given access info.
 * If a token doesn't exist or has expired, generate and return a new one.
 */
char *
seaf_web_at_manager_get_access_token (SeafWebAccessTokenManager *mgr,
                                      const char *repo_id,
                                      const char *obj_id,
                                      const char *op,
                                      const char *username,
                                      int use_onetime,
                                      GError **error);

/*
 * Returns access info for the given token.
 */
SeafileWebAccess *
seaf_web_at_manager_query_access_token (SeafWebAccessTokenManager *mgr,
                                        const char *token);

#ifdef FULL_FEATURE
/* Atomically claims a path-bound ticket and rechecks current native target and
 * authority. No legacy fallback. Returned metadata is not a long-lived grant
 * or a per-block read guard; fileserver release must still be connected. */
SeafileWebAccess *seaf_web_at_manager_consume_read_ticket (
    SeafWebAccessTokenManager *mgr, const char *token, GError **error);
/* Internal single-file path/head/object-bound ticket. Legacy query explicitly
 * refuses it. No RPC/HTTP registration until the current consumption guard is
 * connected; a signed-off issuance is never a future permission grant. */
char *seaf_web_at_manager_issue_read_ticket (SeafWebAccessTokenManager *mgr,
    const char *repo_id, const char *path, const char *head_id, const char *object_id,
    const char *op, const char *username, const char *conditions, GError **error);
#endif

#endif /* WEB_ACCESSTOKEN_MGR_H */
