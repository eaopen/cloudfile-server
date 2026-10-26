#!/usr/bin/env python3
"""Run upstream native integration with the actual fresh CloudFile V2 schema.

Keep upstream build/test orchestration intact. The V2 read guard fails closed
when cf_dir_acl is missing, so a CE-only fixture database is not a valid runtime
for this fork; initialize the same additive DDL shipped by clean CE14 builds.
"""
from functools import partial
import os
from pathlib import Path

import run as upstream
from utils import shell, setup_logging


class CloudFileServerCtl(upstream.ServerCtl):
    def create_database_tables(self):
        super().create_database_tables()
        if self.db == 'mysql':
            ddl = Path(self.sql_dir) / 'mysql' / 'cloudfile-v2.sql'
            shell('sudo mysql -u root -proot',
                  inputdata=('USE seafile; source ' + str(ddl) + ';').encode())


if __name__ == '__main__':
    upstream.ServerCtl = CloudFileServerCtl
    # V2 deliberately refuses C HTTP startup; still build native C and run the
    # complete integration suite against its supported Go fileserver.
    upstream.start_and_test_with_db = partial(
        upstream.start_and_test_with_db, fileservers=('go_fileserver',))
    os.chdir(upstream.TOPDIR)
    setup_logging()
    upstream.main()
