/* Production barrier/dispatcher; fake config and database only, no network. */
#include <glib.h>
#include <string.h>
#include "cf-fileop.h"
#include "cf-lock.h"
#include "seafile-error.h"
#include "log.h"
typedef int SeafDB;
typedef int SeafDBRow;
typedef int SeafDBTrans;
static gboolean transaction_error;
static SeafDB database;
static gboolean checkout;
static int sql_calls, exact_rows, descendant_rows;
static gboolean cf_ext_config_bool(const char *key)
{
    /* A stale file_lock_enabled=true must not activate Checkout. */
    return !strcmp(key, "checkout_enabled") ? checkout : TRUE;
}
static char *cf_ext_config_string(const char *key) { return NULL; }
static SeafDB *cf_ext_database(void) { return &database; }
static int seaf_db_statement_foreach_row(SeafDB *db, const char *sql,
    gboolean (*callback)(SeafDBRow *, void *), void *data, int count, ...)
{
    ++sql_calls;
    g_assert_nonnull(strstr(sql,"FROM cf_edit_guard"));
    g_assert_nonnull(strstr(sql,"cf_edit_guard_active"));
    g_assert_nonnull(strstr(sql,"lease_expires_at>UTC_TIMESTAMP(6)"));
    g_assert_null(strstr(sql,"cf_resource"));
    g_assert_null(strstr(sql,"cf_schema_migration"));
    if (strstr(sql,"path_hash=UNHEX(?)")) return exact_rows;
    g_assert_nonnull(strstr(sql,"path LIKE ?"));
    return descendant_rows;
}
static gboolean seaf_db_trans_check_for_existence(SeafDBTrans *trans,
    const char *sql, gboolean *error, int count, ...)
{
    ++sql_calls;
    g_assert_nonnull(strstr(sql,"FROM cf_edit_guard"));
    g_assert_nonnull(strstr(sql,"path_hash=UNHEX(?)"));
    g_assert_nonnull(strstr(sql,"FOR UPDATE"));
    g_assert_null(strstr(sql,"cf_resource"));
    *error = transaction_error;
    return exact_rows > 0;
}
#include "production.c"

static int write_path(const char *path, GError **error)
{
    CfFileOp op = {.op=CF_OP_UPDATE_FILE,.repo_id="fixture",.dir=path,.user="owner"};
    return cf_fileop_prepare(&op,error);
}
int main(void)
{
    GError *error = NULL;
    checkout=FALSE; cf_lock_init();
    g_assert_false(cf_lock_enabled());
    g_assert_cmpint(write_path("/file",&error),==,0);
    g_assert_cmpint(sql_calls,==,0);
    checkout=TRUE; cf_lock_init();
    g_assert_true(cf_lock_enabled());
    g_assert_cmpint(write_path("/file",&error),==,0);
    g_assert_cmpint(sql_calls,==,2);
    exact_rows=1;
    g_assert_cmpint(write_path("/file",&error),==,-1);
    g_assert_error(error,SEAFILE_DOMAIN,CF_ERR_FILE_LOCKED); g_clear_error(&error);
    exact_rows=0; descendant_rows=1;
    g_assert_cmpint(write_path("/parent",&error),==,-1);
    g_assert_error(error,SEAFILE_DOMAIN,CF_ERR_FILE_LOCKED); g_clear_error(&error);
    descendant_rows=0; exact_rows=-1;
    g_assert_cmpint(write_path("/file",&error),==,-1);
    g_assert_error(error,SEAFILE_DOMAIN,SEAF_ERR_GENERAL); g_clear_error(&error);
    exact_rows=0; descendant_rows=-1;
    g_assert_cmpint(write_path("/parent",&error),==,-1);
    g_assert_error(error,SEAFILE_DOMAIN,SEAF_ERR_GENERAL); g_clear_error(&error);
    checkout=FALSE; cf_fileop_reset(); cf_lock_init(); sql_calls=0;
    g_assert_cmpint(write_path("/file",&error),==,0);
    g_assert_cmpint(sql_calls,==,0);
    /* The final Branch transaction uses the real production function too. */
    g_assert_cmpint(cf_policy_check_unleased_write(NULL,"fixture","/file","owner"),==,0);
    g_assert_cmpint(sql_calls,==,0);
    checkout=TRUE; cf_lock_init(); exact_rows=0;
    g_assert_cmpint(cf_policy_check_unleased_write(NULL,"fixture","/file","owner"),==,0);
    exact_rows=1;
    g_assert_cmpint(cf_policy_check_unleased_write(NULL,"fixture","/file","owner"),==,-1);
    exact_rows=0; transaction_error=TRUE;
    g_assert_cmpint(cf_policy_check_unleased_write(NULL,"fixture","/file","owner"),==,-1);
    transaction_error=FALSE;
    g_assert_cmpint(cf_policy_check_unleased_write(NULL,"fixture","relative","owner"),==,-1);
    g_print("PASS 12 native Checkout barrier and final-transaction cases\n");
    return 0;
}
