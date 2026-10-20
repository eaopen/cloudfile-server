/* Compile the real backend, not a duplicate storage implementation. */
#include "../../common/block-backend-fs.c"
static const char *repo="11111111-1111-4111-8111-111111111111";
static const char *oid="abcdef0123456789012345678901234567890123";
static gboolean count(const char *r,int v,const char *id,void *p) {g_assert_cmpstr(id,==,oid);(*(int*)p)++;return TRUE;}
int main(int argc,char **argv) {
 g_assert_cmpint(argc,==,2);char *legacy=g_build_filename(argv[1],"tmpfiles",NULL);
 g_assert_true(g_file_set_contents(legacy,"poison",-1,NULL));
 BlockBackend *b=block_backend_fs_new(argv[1],legacy);g_assert_nonnull(b);
 BHandle *h=b->open_block(b,repo,1,oid,BLOCK_WRITE);g_assert_nonnull(h);
 char final[SEAF_PATH_MAX];g_assert_nonnull(get_block_path(b,oid,final,repo,1));
 char *parent=g_path_get_dirname(final),*actual=g_path_get_dirname(h->tmp_file);
 g_assert_cmpstr(parent,==,actual);g_assert_false(g_file_test(final,G_FILE_TEST_EXISTS));
 g_assert_cmpint(b->write_block(b,h,"complete",8),==,8);
 int n=0;g_assert_cmpint(b->foreach_block(b,repo,1,count,&n),==,0);g_assert_cmpint(n,==,0);
 g_assert_cmpint(b->close_block(b,h),==,0);g_assert_cmpint(b->commit_block(b,h),==,0);b->block_handle_free(b,h);
 char *orphan=g_build_filename(parent,".cf-tmp-crashed",NULL),*old=g_strconcat(final,".XXXXXX",NULL);
 g_assert_true(g_file_set_contents(orphan,"partial",-1,NULL));g_assert_true(g_file_set_contents(old,"partial",-1,NULL));
 n=0;g_assert_cmpint(b->foreach_block(b,repo,1,count,&n),==,0);g_assert_cmpint(n,==,1);
 h=b->open_block(b,repo,1,oid,BLOCK_WRITE);g_assert_nonnull(h);char *aborted=g_strdup(h->tmp_file);
 b->close_block(b,h);b->block_handle_free(b,h);g_assert_false(g_file_test(aborted,G_FILE_TEST_EXISTS));
 char *data=NULL;gsize len;g_assert_true(g_file_get_contents(final,&data,&len,NULL));g_assert_cmpmem(data,len,"complete",8);
 /* Store removal must report a retained staging file, not unlink it. */
 g_assert_cmpint(b->remove_store(b,repo,NULL,NULL),==,-1);g_assert_true(g_file_test(orphan,G_FILE_TEST_EXISTS));
 return 0;
}
