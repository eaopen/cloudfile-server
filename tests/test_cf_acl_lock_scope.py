"""Cheap source contract for repository-scoped ACL final locking reads.

Not a replacement for the disposable MariaDB EXPLAIN/lock-wait regression:
verify zero-rule and virtual-origin cases with concurrent ACL publication.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ACLFinalLockScope(unittest.TestCase):
    def test_exact_library_pk_range_replaces_cross_library_or_scan(self):
        source = (ROOT / "common/cf-v2-acl.c").read_text(encoding="utf-8")
        part = source.split("static const char *query =", 1)[1].split(
            "/* Native transfer and Hub ACL mutations", 1)[0]
        self.assertIn("FROM cf_dir_acl FORCE INDEX (PRIMARY) WHERE repo_id=? LIMIT 1", part)
        self.assertNotIn("OR repo_id IN", part)
        self.assertIn('g_strconcat(query, " FOR UPDATE", NULL)', part)

    def test_virtual_origin_and_sql_failure_still_fail_closed(self):
        source = (ROOT / "common/cf-v2-acl.c").read_text(encoding="utf-8")
        part = source.split("static int check(", 1)[1].split(
            "/* Native transfer and Hub ACL mutations", 1)[0]
        self.assertIn("SELECT origin_repo FROM VirtualRepo WHERE repo_id=?", source)
        self.assertIn("if (count < 0 || count > 1", part)
        self.assertIn("result = check_scope(trans, session, origin)", part)
        self.assertIn("return -1;", part)

    def test_lock_order_and_acl_writer_contract_unchanged(self):
        branch = (ROOT / "common/branch-mgr.c").read_text(encoding="utf-8")
        update = branch.split("test_and_update_branch (", 1)[1]
        self.assertLess(update.index("cf_v2_acl_proof(NULL"),
                        update.index("AND repo_id=? FOR UPDATE"))
        self.assertLess(update.index("cf_v2_acl_preflight_scoped("),
                        update.index("AND repo_id=? FOR UPDATE"))
        self.assertLess(update.index("AND repo_id=? FOR UPDATE"),
                        update.index("cf_v2_acl_proof(trans"))
        self.assertNotIn("cf_v2_acl_final_scoped (trans", update)


if __name__ == "__main__":
    unittest.main()
