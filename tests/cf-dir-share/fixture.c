/* Real share-qualification code; only the Hub channel is fake. */
#include <glib.h>
#include <jansson.h>
#include <string.h>

/* Exercise the extracted production registration path as well as the grant:
 * only database/schema detection and the internal HTTP channel are stubbed. */
typedef struct SeafDB { int unused; } SeafDB;
#define SEAF_DB_TYPE_MYSQL 1
struct _SeafileSession {
    SeafDB *db;
    const char *seahub_url;
    const char *seahub_pk;
};
static SeafDB fake_db;
static struct _SeafileSession session = { &fake_db, "https://hub.test", "internal-jwt" };
static struct _SeafileSession *seaf = &session;
static int schema_present, schema_fault, db_kind = SEAF_DB_TYPE_MYSQL, registrations;

static int seaf_db_type(SeafDB *db) { (void)db; return db_kind; }
static gboolean seaf_db_statement_exists(SeafDB *db, const char *sql, gboolean *error, int n)
{
    (void)db; (void)n;
    /* The share-qualification adapter checks the minimum V2 identity/ACL
     * dependencies. The complete CloudFile extension schema has five tables. */
    if (!strstr(sql, "SELECT 1 FROM information_schema.tables") ||
        !strstr(sql, "'cf_library_storage'") || !strstr(sql, "'cf_dir_acl'") ||
        !strstr(sql, "'cf_sso_group_map'") ||
        !strstr(sql, "COUNT(DISTINCT table_name)=3"))
        return FALSE;
    if (schema_fault) { *error = TRUE; return FALSE; }
    return schema_present != 0;
}
static void seaf_warning(const char *format, ...) { (void)format; }
char *cf_v2_share_grant(const char *, const char *, const char *, gboolean *);
static void cf_ext_register_share_grant(const char *name,
    char *(*grant)(const char *, const char *, const char *, gboolean *))
{
    if (strcmp(name, "v2-dir-share") == 0 && grant == cf_v2_share_grant)
        ++registrations;
}

static int channel_status;
static char *channel_body;
static char *last_payload;
static int calls;

long cf_v2_acl_check_json(struct _SeafileSession *ignored, const char *payload, GString *body);

/* Stands in for cf-v2-acl.c's transport. It never reaches the network: the
 * status and body each test wants are exactly what a Hub reply or an outage
 * would hand back. */
long cf_v2_acl_check_json(struct _SeafileSession *ignored, const char *payload, GString *body)
{
    (void)ignored;
    ++calls;
    g_free(last_payload);
    last_payload = g_strdup(payload);
    if (channel_status == 200 && channel_body)
        g_string_append(body, channel_body);
    return channel_status;
}

#include "production.c"

void fixture_reset(void)
{
    channel_status = 403;
    g_clear_pointer(&channel_body, g_free);
    channel_body = g_strdup("{\"code\":\"ACL_DENIED\"}");
    g_clear_pointer(&last_payload, g_free);
    calls = 0;
    schema_present = schema_fault = registrations = 0;
    db_kind = SEAF_DB_TYPE_MYSQL;
    session.db = &fake_db;
    session.seahub_url = "https://hub.test";
    session.seahub_pk = "internal-jwt";
}
void fixture_registration_context(int present, int kind, int channel, int fault)
{
    schema_present = present;
    db_kind = kind;
    schema_fault = fault;
    registrations = 0;
    session.seahub_url = channel ? "https://hub.test" : NULL;
    session.seahub_pk = channel ? "internal-jwt" : NULL;
}
int fixture_registrations(void) { return registrations; }

void fixture_reply(int status, const char *body)
{
    channel_status = status;
    g_clear_pointer(&channel_body, g_free);
    channel_body = body ? g_strdup(body) : NULL;
}

int fixture_calls(void) { return calls; }
const char *fixture_payload(void) { return last_payload; }
void fixture_free(void *value) { g_free(value); }
