/* Real share-qualification code; only the Hub channel is fake. */
#include <glib.h>
#include <jansson.h>
#include <string.h>

/* cf-v2-share.c only passes this pointer through to the channel, so the
 * fixture owns the whole type. */
struct _SeafileSession { int unused; };
static struct _SeafileSession session;
static struct _SeafileSession *seaf = &session;

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
}

void fixture_reply(int status, const char *body)
{
    channel_status = status;
    g_clear_pointer(&channel_body, g_free);
    channel_body = body ? g_strdup(body) : NULL;
}

int fixture_calls(void) { return calls; }
const char *fixture_payload(void) { return last_payload; }
void fixture_free(void *value) { g_free(value); }
