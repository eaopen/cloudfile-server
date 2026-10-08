#include <assert.h>
#include "cf-auto-storage.h"

/* Drive the real C policy with the same fixtures as Go and Python. */
int main (int argc, char **argv)
{
    char *id;
    gboolean valid;
    assert (argc == 3);
    id = g_strconcat (CF_AUTO_STORAGE_PREFIX, argv[1], NULL);
    valid = strcmp (argv[2], "true") == 0;
    assert (cf_auto_key_valid (argv[1]) == valid);
    assert ((cf_auto_storage_key (id) != NULL) == valid);
    g_free (id);
    return 0;
}
