/* Exercise cleanup and enumeration using the real metadata backend. */
#include "../../common/obj-backend-fs.c"
extern int fail_write;
static const char *repo="11111111-1111-4111-8111-111111111111";
static const char *oid="abcdef0123456789012345678901234567890123";
static gboolean count(const char *r,int v,const char *id,void *p) {g_assert_cmpstr(id,==,oid);(*(int*)p)++;return TRUE;}
int main(int argc,char **argv) {
 g_assert_cmpint(argc,==,2);ObjBackend *b=obj_backend_fs_new(argv[1],"fs");g_assert_nonnull(b);
 g_assert_cmpint(b->write(b,repo,1,oid,"complete",8,TRUE),==,0);
 char final[SEAF_PATH_MAX];g_assert_cmpint(id_to_path(b->priv,oid,final,repo,1),==,0);
 fail_write=1;g_assert_cmpint(b->write(b,repo,1,oid,"partial",7,TRUE),==,-1);fail_write=0;
 char *parent=g_path_get_dirname(final);GDir *dir=g_dir_open(parent,0,NULL);const char *name;int n=0;
 while ((name=g_dir_read_name(dir))) {g_assert_false(g_str_has_prefix(name,".cf-tmp-"));n++;}g_dir_close(dir);g_assert_cmpint(n,==,1);
 char *orphan=g_build_filename(parent,".cf-tmp-crashed",NULL),*old=g_strconcat(final,".XXXXXX",NULL);
 g_assert_true(g_file_set_contents(orphan,"partial",-1,NULL));g_assert_true(g_file_set_contents(old,"partial",-1,NULL));
 n=0;g_assert_cmpint(b->foreach_obj(b,repo,1,count,&n),==,0);g_assert_cmpint(n,==,1);
 char *data=NULL;gsize len;g_assert_true(g_file_get_contents(final,&data,&len,NULL));g_assert_cmpmem(data,len,"complete",8);
 if (g_getenv("CF_TEST_V2_STORAGE_ROOT")) {
  char *native=g_build_filename(argv[1],"storage","fs",repo,"ab",oid+2,NULL);
  g_assert_false(g_file_test(native,G_FILE_TEST_EXISTS));g_free(native);
 }
 /* Unknown shard names and both old/new staging formats stay untouched. */
 char *root=g_path_get_dirname(parent),*unknown_dir=g_build_filename(root,"zz",NULL);
 char *unknown=g_build_filename(unknown_dir,"do-not-delete",NULL);
 g_assert_cmpint(g_mkdir_with_parents(unknown_dir,0700),==,0);
 g_assert_true(g_file_set_contents(unknown,"foreign",-1,NULL));
 g_assert_cmpint(b->remove_store(b,repo,NULL,NULL),==,-1);
 g_assert_true(g_file_test(orphan,G_FILE_TEST_EXISTS));
 g_assert_true(g_file_test(old,G_FILE_TEST_EXISTS));
 g_assert_true(g_file_test(unknown,G_FILE_TEST_EXISTS));
 return 0;
}
