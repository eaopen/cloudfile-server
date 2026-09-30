#!/bin/sh
# Disposable fixture build, including test-only counters; do not package.
set -eu
cp -a /configured /tmp/server
# The current RPC and Makefile also reference the directory pager. Copy its
# scan/header dependencies together to avoid linking against a stale snapshot.
cp /source/common/cf-acl.c /source/common/cf-acl-resolve.c /source/common/cf-ext.c /source/common/rpc-service.c /source/common/cf-permission-many.* /source/common/cf-dir-page.* /tmp/server/common/
cp /source/include/seafile-rpc.h /tmp/server/include/
cp /source/server/Makefile.am /source/server/seaf-server.c /source/server/repo-perm.c /source/server/repo-mgr.h /tmp/server/server/
cd /tmp/server
# Test-only call counters: never copied into the working tree or release artifact.
python3 - <<'INSTRUMENT'
from pathlib import Path
for file, name, label in [('common/rpc-service.c','seafile_check_permission_by_path','scalar'), ('common/cf-acl-resolve.c','cf_acl_resolve','evaluate')]:
 p=Path(file);s=p.read_text();begin=s.index('\n'+name+' (');body=s.index('{',begin)+1
 s=s[:body]+'\n    fprintf(stderr, "CF_TEST_'+label+'\\n");\n'+s[body:]
 p.write_text('#include <stdio.h>\n'+s)
INSTRUMENT
./autogen.sh
./configure --prefix=/work/build/cloudfile_14.0/seafile-server/seafile --with-mysql=/usr/bin/mariadb_config 'LDFLAGS=-L/work/build/cloudfile_14.0/seafile-server/seafile/lib -L/evhtp' 'CPPFLAGS=-I/work/build/cloudfile_14.0/seafile-server/seafile/include -I/evhtp -I/evhtp/evthr -I/evhtp/htparse -I/evhtp/build' PKG_CONFIG_PATH=/work/build/cloudfile_14.0/seafile-server/seafile/lib/pkgconfig
make -j6 -C lib
make -j6 -C server
cp server/.libs/seaf-server /lab/seaf-server
