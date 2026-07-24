
AM_CFLAGS = -DPKGDATADIR=\"$(pkgdatadir)\" \
	-DPACKAGE_DATA_DIR=\""$(pkgdatadir)"\" \
	-DSEAFILE_SERVER \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/lib \
	-I$(top_builddir)/lib \
	-I$(top_srcdir)/common \
	@SEARPC_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@JANSSON_CFLAGS@ \
	@CURL_CFLAGS@ \
	@MSVC_CFLAGS@ \
	@MYSQL_CFLAGS@ \
	-Wall

bin_PROGRAMS = seafserv-gc seaf-fsck seaf-storage-migrate

noinst_HEADERS = \
	seafile-session.h \
	repo-mgr.h \
	verify.h \
	fsck.h \
	gc-core.h \
	../../common/cf-s3-client.h \
	../../common/storage-backend-multi.h

common_sources = \
	seafile-session.c \
	repo-mgr.c \
	../../common/seaf-db.c \
	../../common/branch-mgr.c \
	../../common/fs-mgr.c \
	../../common/block-mgr.c \
	../../common/block-backend.c \
	../../common/block-backend-fs.c \
	../../common/block-backend-s3.c \
	../../common/commit-mgr.c \
	../../common/log.c \
	../../common/seaf-utils.c \
	../../common/obj-store.c \
	../../common/obj-backend-fs.c \
	../../common/obj-backend-s3.c \
	../../common/cf-s3-client.c \
	../../common/storage-backend-multi.c \
	../../common/seafile-crypt.c \
	../../common/password-hash.c \
	../../common/config-mgr.c

seafserv_gc_SOURCES = \
	seafserv-gc.c \
	verify.c \
	gc-core.c \
	$(common_sources)

seafserv_gc_LDADD = $(top_builddir)/common/cdc/libcdc.la \
	$(top_builddir)/lib/libseafile_common.la \
	@GLIB2_LIBS@ @GOBJECT_LIBS@ @SSL_LIBS@ @LIB_RT@ @LIB_UUID@ -lsqlite3 @LIBEVENT_LIBS@ \
	@SEARPC_LIBS@ @JANSSON_LIBS@ ${LIB_WS32} @ZLIB_LIBS@ \
	@MYSQL_LIBS@ -lsqlite3 @ARGON2_LIBS@ @CURL_LIBS@

seaf_fsck_SOURCES = \
	seaf-fsck.c \
	fsck.c \
	$(common_sources)

seaf_fsck_LDADD = $(top_builddir)/common/cdc/libcdc.la \
	$(top_builddir)/lib/libseafile_common.la \
	@GLIB2_LIBS@ @GOBJECT_LIBS@ @SSL_LIBS@ @LIB_RT@ @LIB_UUID@ -lsqlite3 @LIBEVENT_LIBS@ \
	@SEARPC_LIBS@ @JANSSON_LIBS@ ${LIB_WS32} @ZLIB_LIBS@ \
	@MYSQL_LIBS@ -lsqlite3 @ARGON2_LIBS@ @CURL_LIBS@

seaf_storage_migrate_SOURCES = \
	seaf-storage-migrate.c \
	$(common_sources)

seaf_storage_migrate_LDADD = $(top_builddir)/common/cdc/libcdc.la \
	$(top_builddir)/lib/libseafile_common.la \
	@GLIB2_LIBS@ @GOBJECT_LIBS@ @SSL_LIBS@ @LIB_RT@ @LIB_UUID@ -lsqlite3 @LIBEVENT_LIBS@ \
	@SEARPC_LIBS@ @JANSSON_LIBS@ ${LIB_WS32} @ZLIB_LIBS@ \
	@MYSQL_LIBS@ -lsqlite3 @ARGON2_LIBS@ @CURL_LIBS@
