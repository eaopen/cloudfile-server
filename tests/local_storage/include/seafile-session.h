/* Only the fields used by cf-v2-storage.c; no database is opened by these tests. */
#include "seaf-db.h"
typedef struct {
    GKeyFile *config;
    SeafDB *db;
} SeafileSession;
extern SeafileSession *seaf;
