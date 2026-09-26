/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "cloudfile-acl.h"
#include <stdlib.h>
#include <string.h>

/* Input is an indexed ancestor candidate set, never all rules for all files. */
#define CF_ACL_LIMIT 4096
#define CF_PATH_LIMIT 4096

static int canonical(const char *path)
{
    const char *segment;
    size_t length;
    if (!path || path[0] != '/') return 0;
    length = strlen(path);
    if (length > CF_PATH_LIMIT || (length > 1 && path[length - 1] == '/')) return 0;
    if (length == 1) return 1;
    segment = path + 1;
    for (const char *p = segment; ; ++p) {
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t)(p - segment);
            if (n == 0 || (n == 1 && segment[0] == '.') ||
                (n == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return 1;
}

static int ancestor(const char *parent, const char *path)
{
    size_t n = strlen(parent);
    return strcmp(parent, "/") == 0 ||
        (strncmp(parent, path, n) == 0 && (path[n] == '\0' || path[n] == '/'));
}

static int contains(const char *const *items, size_t count, const char *id)
{
    for (size_t i = 0; i < count; ++i)
        if (strcmp(items[i], id) == 0) return 1;
    return 0;
}

static int member(const struct cf_acl_context *ctx, const struct cf_acl_rule *r)
{
    if (r->subject_type == CF_USER) return strcmp(ctx->user_id, r->subject_id) == 0;
    if (r->subject_type == CF_DEPT) return contains(ctx->departments, ctx->department_count, r->subject_id);
    return contains(ctx->groups, ctx->group_count, r->subject_id);
}

static int compare(const void *a, const void *b)
{
    const struct cf_acl_rule *x = *(const struct cf_acl_rule *const *)a;
    const struct cf_acl_rule *y = *(const struct cf_acl_rule *const *)b;
    int diff = x->subject_type - y->subject_type;
    if (diff) return diff;
    diff = strcmp(x->subject_id, y->subject_id);
    if (diff) return diff;
    /* Sorting by depth selects the deepest applicable rule per subject. */
    size_t nx = strlen(x->path), ny = strlen(y->path);
    if (nx != ny) return nx < ny ? -1 : 1;
    return x->permission - y->permission;
}

static int resolve(const struct cf_acl_context *ctx, const char *path,
                   const struct cf_acl_rule **candidates, size_t count)
{
    int winning_type = 0, permission = ctx->ce_permission;
    for (size_t i = 0; i < count; ) {
        size_t end = i;
        const struct cf_acl_rule *best = NULL;
        while (end < count && candidates[end]->subject_type == candidates[i]->subject_type &&
               strcmp(candidates[end]->subject_id, candidates[i]->subject_id) == 0) {
            const struct cf_acl_rule *r = candidates[end++];
            if (strcmp(r->path, path) == 0 ||
                (r->inherit && r->kind == CF_DIRECTORY && ancestor(r->path, path))) {
                /* At equal depth conflicting duplicate rules fail closed. */
                if (best && strlen(best->path) == strlen(r->path) && best->permission != r->permission)
                    return -1;
                best = r;
            }
        }
        if (best) {
            if (best->subject_type > winning_type) {
                winning_type = best->subject_type;
                permission = best->permission;
            } else if (best->subject_type == winning_type) {
                if (permission <= CF_NONE || best->permission <= CF_NONE)
                    permission = permission < best->permission ? permission : best->permission;
                else
                    permission = permission > best->permission ? permission : best->permission;
            }
        }
        i = end;
    }
    return permission;
}

int cf_acl_evaluate(const struct cf_acl_context *ctx, const char *path, int kind,
                    const struct cf_acl_rule *rules, size_t count, struct cf_acl_result *out)
{
    const struct cf_acl_rule **candidates;
    size_t selected = 0;
    int permission;
    char parent[CF_PATH_LIMIT + 1];
    if (!out) return -1;
    *out = (struct cf_acl_result){0, 0, 0};
    if (!ctx || !ctx->user_id || !*ctx->user_id || !canonical(path) ||
        (kind != CF_DIRECTORY && kind != CF_FILE) || (kind == CF_FILE && strcmp(path, "/") == 0) ||
        count > CF_ACL_LIMIT || (count && !rules) ||
        ctx->department_count > CF_ACL_LIMIT || ctx->group_count > CF_ACL_LIMIT ||
        (ctx->department_count && !ctx->departments) || (ctx->group_count && !ctx->groups)) return -1;
    for (size_t i = 0; i < ctx->department_count; ++i)
        if (!ctx->departments[i] || !*ctx->departments[i]) return -1;
    for (size_t i = 0; i < ctx->group_count; ++i)
        if (!ctx->groups[i] || !*ctx->groups[i]) return -1;
    if (!ctx->ready || !ctx->active || ctx->barrier_active ||
        (ctx->ce_permission != CF_READ && ctx->ce_permission != CF_WRITE)) return 0;
    candidates = malloc((count ? count : 1) * sizeof(*candidates));
    if (!candidates) return -1;
    for (size_t i = 0; i < count; ++i) {
        const struct cf_acl_rule *r = &rules[i];
        if (!canonical(r->path) || !r->subject_id || !*r->subject_id ||
            r->subject_type < CF_GROUP || r->subject_type > CF_USER ||
            r->permission < CF_INVISIBLE || r->permission > CF_WRITE ||
            (r->inherit != 0 && r->inherit != 1) ||
            (r->kind != CF_DIRECTORY && r->kind != CF_FILE) ||
            (r->kind == CF_FILE && (r->permission > CF_NONE || strcmp(r->path, "/") == 0))) {
            free(candidates); return -1;
        }
        if (member(ctx, r) && ancestor(r->path, path)) candidates[selected++] = r;
    }
    qsort(candidates, selected, sizeof(*candidates), compare);
    /* Evaluate ancestors separately: a child grant cannot reveal a hidden parent. */
    strcpy(parent, path);
    for (char *slash = strrchr(parent, '/'); slash; slash = strrchr(parent, '/')) {
        if (slash == parent) parent[1] = '\0'; else *slash = '\0';
        permission = resolve(ctx, parent, candidates, selected);
        if (permission <= CF_INVISIBLE) { free(candidates); return permission < 0 ? -1 : 0; }
        if (slash == parent) break;
    }
    permission = resolve(ctx, path, candidates, selected);
    free(candidates);
    if (permission < 0) return -1;
    out->visible = permission != CF_INVISIBLE;
    out->read = permission >= CF_READ;
    out->write = permission == CF_WRITE && !ctx->hard_readonly;
    return 0;
}
