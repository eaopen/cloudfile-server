"""CF-EDIT's real MariaDB transaction gate; run only on disposable database.

Usage: CF_EDIT_SQL=<path> CF_EDIT_MYSQL_* env python3 -m unittest tests.cf_edit.test_mysql
This does NOT stand in for Seafile Native C/Go integration E2E.
"""
import hashlib
import time
import os
from pathlib import Path
from threading import Event, Thread
from unittest import TestCase, skipUnless

try:
    import pymysql
except ImportError:
    pymysql = None


def connection():
    if pymysql is None:
        raise RuntimeError("pymysql required")
    return pymysql.connect(
        host=os.getenv("CF_EDIT_MYSQL_HOST", "127.0.0.1"),
        port=int(os.getenv("CF_EDIT_MYSQL_PORT", "3306")),
        user=os.getenv("CF_EDIT_MYSQL_USER", "root"),
        password=os.getenv("CF_EDIT_MYSQL_PASSWORD", "test"),
        database=os.getenv("CF_EDIT_MYSQL_DB", "seafile_db"),
        autocommit=False,
        connect_timeout=5,
    )


REPO = "11111111-1111-4111-8111-111111111111"
PATH = "/Engineering/part.step"
HASH = hashlib.sha256(PATH.encode()).digest()


# This suite creates/drops Branch and Guard tables, so it must use its own
# explicitly selected disposable DB, never the upstream native test database.
@skipUnless(os.environ.get('CF_EDIT_MYSQL_DB'), 'requires dedicated CF_EDIT_MYSQL_DB fixture')
class MariaDBFeatureTests(TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = connection()
        with cls.db.cursor() as c:
            # Owned only by this disposable DB/CI job.
            c.execute("CREATE TABLE Branch (repo_id CHAR(36) NOT NULL, name VARCHAR(10) NOT NULL,"
                      "commit_id CHAR(40) NOT NULL, PRIMARY KEY(repo_id,name)) ENGINE=InnoDB")
        cls.db.commit()
        ddl = Path(os.getenv("CF_EDIT_SQL", "scripts/sql/mysql/cloudfile-v2-edit.sql")).read_text()
        # Strip SQL comments BEFORE splitting statements: comments can contain
        # literal semicolons that are not SQL statement delimiters.
        ddl = "\n".join(line for line in ddl.splitlines() if not line.lstrip().startswith("--"))
        with cls.db.cursor() as c:
            for clause in ddl.split(";"):
                if clause.strip():
                    c.execute(clause)
            c.execute("INSERT INTO Branch VALUES(%s,'master',%s)", (REPO, "a" * 40))
        cls.db.commit()

    @classmethod
    def tearDownClass(cls):
        try:
            with cls.db.cursor() as c:
                c.execute("DROP TABLE cf_edit_guard")
                c.execute("DROP TABLE Branch")
            cls.db.commit()
        finally:
            cls.db.close()

    def tearDown(self):
        with self.db.cursor() as c:
            c.execute("DELETE FROM cf_edit_guard")
            c.execute("UPDATE Branch SET commit_id=%s", ("a" * 40,))
        self.db.commit()

    def test_one_table_guard_slot_owner_and_terminal(self):
        with self.db.cursor() as c:
            c.execute("INSERT INTO cf_edit_guard(repo_id,path_hash,path) VALUES(%s,%s,%s)", (REPO,HASH,PATH))
            c.execute("UPDATE cf_edit_guard SET guard_id=%s,generation=1,"
                      "owner_uid='user1',owner_native_user='user@auth.local',"
                      "terminal_type='windows',terminal_name='design-laptop',"
                      "checkout_ip='10.9.8.58',proof_digest=%s,base_file_id=%s,"
                      "lease_expires_at=TIMESTAMPADD(SECOND,600,UTC_TIMESTAMP(6)),"
                      "created_at=UTC_TIMESTAMP(6) WHERE repo_id=%s AND path_hash=%s",
                      (REPO,"f"*64,"a"*40,REPO,HASH))
            c.execute("SELECT generation,guard_id,terminal_type,checkout_ip FROM cf_edit_guard "
                      "WHERE repo_id=%s AND path_hash=%s", (REPO,HASH))
            self.assertEqual(c.fetchone(),(1,REPO,"windows","10.9.8.58"))
            c.execute("UPDATE cf_edit_guard SET guard_id=NULL,owner_uid=NULL,"
                      "owner_native_user=NULL,terminal_type=NULL,terminal_name=NULL,"
                      "checkout_ip=NULL,proof_digest=NULL,base_file_id=NULL,lease_expires_at=NULL "
                      "WHERE repo_id=%s AND path_hash=%s",(REPO,HASH))
            c.execute("SELECT generation,guard_id FROM cf_edit_guard WHERE repo_id=%s",(REPO,))
            self.assertEqual(c.fetchone(),(1,None))
        self.db.commit()

    def test_native_branch_and_checkout_release_commit_or_rollback_together(self):
        with self.db.cursor() as c:
            c.execute("INSERT INTO cf_edit_guard(repo_id,path_hash,path) VALUES(%s,%s,%s)",(REPO,HASH,PATH))
            c.execute("UPDATE cf_edit_guard SET guard_id=%s,generation=1,"
                      "owner_uid='user1',owner_native_user='user@auth.local',"
                      "proof_digest=%s,base_file_id=%s,"
                      "lease_expires_at=TIMESTAMPADD(SECOND,600,UTC_TIMESTAMP(6)) "
                      "WHERE repo_id=%s AND path_hash=%s",
                      (REPO,"f"*64,"a"*40,REPO,HASH))
        self.db.commit()
        with self.db.cursor() as c:
            c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s FOR UPDATE",(REPO,))
            c.execute("UPDATE cf_edit_guard SET guard_id=NULL,owner_uid=NULL,"
                      "owner_native_user=NULL,proof_digest=NULL,base_file_id=NULL,"
                      "lease_expires_at=NULL WHERE repo_id=%s AND path_hash=%s",(REPO,HASH))
            c.execute("UPDATE Branch SET commit_id=%s WHERE repo_id=%s",("c"*40,REPO))
        self.db.rollback()
        with self.db.cursor() as c:
            c.execute("SELECT guard_id FROM cf_edit_guard WHERE repo_id=%s",(REPO,))
            self.assertEqual(c.fetchone()[0],REPO)
            c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s",(REPO,))
            self.assertEqual(c.fetchone()[0],"a"*40)
            c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s FOR UPDATE",(REPO,))
            c.execute("UPDATE cf_edit_guard SET guard_id=NULL,owner_uid=NULL,"
                      "owner_native_user=NULL,proof_digest=NULL,base_file_id=NULL,"
                      "lease_expires_at=NULL WHERE repo_id=%s AND path_hash=%s",(REPO,HASH))
            c.execute("UPDATE Branch SET commit_id=%s WHERE repo_id=%s AND commit_id=%s",
                      ("c"*40,REPO,"a"*40))
            self.assertEqual(c.rowcount,1)
        self.db.commit()
        with self.db.cursor() as c:
            c.execute("SELECT guard_id FROM cf_edit_guard WHERE repo_id=%s",(REPO,))
            self.assertIsNone(c.fetchone()[0])
            c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s",(REPO,))
            self.assertEqual(c.fetchone()[0],"c"*40)

    def test_guard_lookup_scales_with_active_locks_not_history(self):
        """A large dormant Guard history must not be scanned per native write.

        This is a database-index + query-plan regression, not a full throughput
        benchmark of Seafile uploads or C tree-diff operations.
        """
        history = []
        for i in range(12000):
            path = "/Archive/historical-%05d.step" % i
            history.append((REPO, hashlib.sha256(path.encode()).digest(), path))
        with self.db.cursor() as c:
            c.executemany(
                "INSERT INTO cf_edit_guard(repo_id,path_hash,path) VALUES(%s,%s,%s)",
                history)
            c.execute(
                "UPDATE cf_edit_guard SET guard_id=%s,generation=1,"
                "owner_uid='owner',owner_native_user='owner@auth.local',"
                "proof_digest=%s,base_file_id=%s,"
                "lease_expires_at=TIMESTAMPADD(SECOND,600,UTC_TIMESTAMP(6)) "
                "WHERE repo_id=%s AND path_hash=%s",
                (REPO, "f"*64, "a"*40, REPO, history[5000][1]))
        self.db.commit()

        query = ("SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) "
                 "WHERE repo_id=%s AND guard_id IS NOT NULL AND lease_expires_at>UTC_TIMESTAMP(6) FOR UPDATE")
        with self.db.cursor() as c:
            c.execute("EXPLAIN " + query, (REPO,))
            plan = c.fetchone()
            self.assertEqual(plan[5], "cf_edit_guard_active")  # chosen key
            self.assertLess(int(plan[8]), 100)  # estimated active rows
            t0 = time.perf_counter()
            for _ in range(25):
                c.execute(query, (REPO,))
                rows = c.fetchall()
                self.assertEqual(rows, ((history[5000][2],),))
            elapsed = time.perf_counter() - t0
            # Generous CI guardrail only: lookups should take much less than
            # a second each despite 12k historical Guard slots.
            self.assertLess(elapsed, 5.0)
            # Once the only active reservation is released, 12,000 dormant
            # historical rows still must yield an empty indexed range.
            c.execute(
                "UPDATE cf_edit_guard SET guard_id=NULL,owner_uid=NULL,owner_native_user=NULL,"
                "proof_digest=NULL,base_file_id=NULL,lease_expires_at=NULL "
                "WHERE repo_id=%s AND path_hash=%s",
                (REPO, history[5000][1]))
            c.execute(query, (REPO,))
            self.assertEqual(c.fetchall(), ())
        self.db.commit()

    def test_branch_lock_serializes_checkout_and_publish(self):
        locked, attempted, acquired = Event(), Event(), Event()
        errors = []

        def contender():
            db2 = connection()
            try:
                with db2.cursor() as c:
                    c.execute("SET SESSION innodb_lock_wait_timeout=3")
                    attempted.set()
                    c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s FOR UPDATE", (REPO,))
                    acquired.set()
                db2.rollback()
            except Exception as exc:
                errors.append(exc)
            finally:
                db2.close()

        with self.db.cursor() as c:
            c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s FOR UPDATE", (REPO,))
            locked.set()
            t = Thread(target=contender)
            t.start()
            self.assertTrue(attempted.wait(2))
            # A second edit operation cannot acquire the authoritative Branch lock.
            self.assertFalse(acquired.wait(0.2))
        self.db.commit()
        t.join(4)
        self.assertFalse(errors, repr(errors))
        self.assertTrue(acquired.is_set())
