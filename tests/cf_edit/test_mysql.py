"""CF-EDIT's real MariaDB transaction gate; run only on disposable database.

Usage: CF_EDIT_SQL=<path> CF_EDIT_MYSQL_* env python3 -m unittest tests.cf_edit.test_mysql
This does NOT stand in for Seafile Native C/Go integration E2E.
"""
import hashlib
import time
import os
from pathlib import Path
from threading import Event, Thread
from unittest import TestCase

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
                c.execute("DROP TABLE cf_commit_intent")
                c.execute("DROP TABLE cf_edit_guard")
                c.execute("DROP TABLE Branch")
            cls.db.commit()
        finally:
            cls.db.close()

    def tearDown(self):
        with self.db.cursor() as c:
            c.execute("DELETE FROM cf_commit_intent")
            c.execute("DELETE FROM cf_edit_guard")
            c.execute("UPDATE Branch SET commit_id=%s", ("a" * 40,))
        self.db.commit()

    def test_two_tables_and_monotonic_guard_slot(self):
        with self.db.cursor() as c:
            c.execute("INSERT INTO cf_edit_guard(repo_id,path_hash,path) VALUES(%s,%s,%s)", (REPO, HASH, PATH))
            c.execute("UPDATE cf_edit_guard SET guard_id=%s,generation=1,credential_epoch=1,"
                      "mode='checkout',owner_uid='user1',proof_digest=%s,"
                      "checkout_policy='persistent',base_file_id=%s "
                      "WHERE repo_id=%s AND path_hash=%s",
                      (REPO, "f" * 64, "a" * 40, REPO, HASH))
            c.execute("SELECT generation,guard_id,checkout_policy FROM cf_edit_guard "
                      "WHERE repo_id=%s AND path_hash=%s", (REPO, HASH))
            self.assertEqual(c.fetchone(), (1, REPO, "persistent"))
            c.execute("UPDATE cf_edit_guard SET guard_id=NULL,mode=NULL,owner_uid=NULL,"
                      "proof_digest=NULL,checkout_policy=NULL,base_file_id=NULL,"
                      "credential_epoch=0 WHERE repo_id=%s AND path_hash=%s", (REPO, HASH))
            c.execute("SELECT generation,guard_id FROM cf_edit_guard WHERE repo_id=%s", (REPO,))
            self.assertEqual(c.fetchone(), (1, None))
        self.db.commit()

    def test_intent_receipt_rolls_back_with_native_branch(self):
        with self.db.cursor() as c:
            c.execute("INSERT INTO cf_commit_intent(intent_id,repo_id,path_hash,path,actor_uid,"
                      "action,expected_file_id,staged_file_id,content_digest,request_digest,"
                      "state) VALUES(%s,%s,%s,%s,'user1','optimistic',%s,%s,%s,%s,'prepared')",
                      (REPO, REPO, HASH, PATH, "a" * 40, "b" * 40, "0" * 64, "1" * 64))
        self.db.commit()
        with self.db.cursor() as c:
            c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s AND name='master' FOR UPDATE", (REPO,))
            c.execute("UPDATE cf_commit_intent SET state='published',"
                      "result_file_id=%s,result_commit_id=%s WHERE intent_id=%s",
                      ("b" * 40, "c" * 40, REPO))
            c.execute("UPDATE Branch SET commit_id=%s WHERE repo_id=%s", ("c" * 40, REPO))
        self.db.rollback()
        with self.db.cursor() as c:
            c.execute("SELECT state,result_commit_id FROM cf_commit_intent WHERE intent_id=%s", (REPO,))
            self.assertEqual(c.fetchone(), ("prepared", None))
            c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s", (REPO,))
            self.assertEqual(c.fetchone(), ("a" * 40,))
            c.execute("SELECT commit_id FROM Branch WHERE repo_id=%s FOR UPDATE", (REPO,))
            c.execute("UPDATE cf_commit_intent SET state='published',result_file_id=%s,"
                      "result_commit_id=%s WHERE intent_id=%s AND state='prepared'",
                      ("b" * 40, "c" * 40, REPO))
            c.execute("UPDATE Branch SET commit_id=%s WHERE repo_id=%s AND commit_id=%s",
                      ("c" * 40, REPO, "a" * 40))
            self.assertEqual(c.rowcount, 1)
        self.db.commit()
        with self.db.cursor() as c:
            c.execute("SELECT state,result_commit_id FROM cf_commit_intent WHERE intent_id=%s", (REPO,))
            self.assertEqual(c.fetchone(), ("published", "c" * 40))

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
                "credential_epoch=1,mode='checkout',owner_uid='owner',"
                "proof_digest=%s,checkout_policy='persistent',base_file_id=%s "
                "WHERE repo_id=%s AND path_hash=%s",
                (REPO, "f"*64, "a"*40, REPO, history[5000][1]))
        self.db.commit()

        query = ("SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) "
                 "WHERE repo_id=%s AND guard_id IS NOT NULL FOR UPDATE")
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
                "UPDATE cf_edit_guard SET guard_id=NULL,mode=NULL,"
                "owner_uid=NULL,proof_digest=NULL,checkout_policy=NULL,"
                "base_file_id=NULL,credential_epoch=0 "
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
