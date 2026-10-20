/* Test-only routing and I/O seams. Backend functions come from real sources. */
#include "common.h"
#include "utils.h"
#include "cf-v2-storage.h"
int fail_write;
ssize_t readn(int fd, void *p, size_t n) { return read(fd,p,n); }
ssize_t writen(int fd, const void *p, size_t n) { if (fail_write) { errno=EIO;return -1; } return write(fd,p,n); }
void string_list_free(GList *list) { g_list_free_full(list,g_free); }
gboolean is_object_id_valid(const char *id) {
 if (!id || strlen(id)!=40) return FALSE;
 for(int i=0;i<40;i++) if (!(g_ascii_isdigit(id[i]) || (id[i]>='a' && id[i]<='f'))) return FALSE;
 return TRUE;
}
char *cf_v2_store_dir(const char *root,const char *repo) { return g_build_filename(root,repo,NULL); }
gboolean cf_v2_storage_enabled(void) { return FALSE; }
int cf_v2_storage_copy(const char *src,const char *dst) { return link(src,dst); }
int seaf_stat(const char *path, SeafStat *st) { return stat(path,st); }
int seaf_fstat(int fd, SeafStat *st) { return fstat(fd,st); }
#include "log.h"
void seafile_debug_impl(SeafileDebugFlags flag,const gchar *format,...) { }
