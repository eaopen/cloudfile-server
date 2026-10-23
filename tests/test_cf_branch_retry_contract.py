"""Cheap regression guard for C Branch CAS return codes and retry boundaries.

This is a source-level contract test. The native ACL integration harness still
requires an isolated MariaDB/Hub/C-server environment before merge.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(path, marker, until):
    source = (ROOT / path).read_text(encoding="utf-8")
    assert marker in source and until in source, path
    return source.split(marker, 1)[1].split(until, 1)[0]


class BranchRetryContract(unittest.TestCase):
    def test_only_head_mismatch_is_classified_retryable(self):
        hdr = (ROOT / "common/branch-mgr.h").read_text()
        branch = function("common/branch-mgr.c",
                          "test_and_update_branch (SeafBranchManager *mgr,",
                          "\nint\nseaf_branch_manager_test_and_update_branch (")
        self.assertIn("#define SEAF_BRANCH_UPDATE_HEAD_CONFLICT (-3)", hdr)
        self.assertIn("return SEAF_BRANCH_UPDATE_HEAD_CONFLICT;", branch)
        self.assertIn("return SEAF_BRANCH_UPDATE_ACL_DENIED;", branch)
        self.assertIn("return -1;", branch)  # SQL/transaction failure stays distinct

    def test_metadata_noop_and_bounded_retry(self):
        method = function("server/repo-mgr.c", "seaf_repo_manager_edit_repo (",
                          "\ngboolean\nget_total_file_number_cb")
        self.assertIn("g_strcmp0 (parent->repo_name, effective_name)", method)
        self.assertIn("g_strcmp0 (parent->repo_desc, effective_description)", method)
        self.assertLess(method.index("g_strcmp0 (parent->repo_name"),
                        method.index("seaf_commit_new ("))
        self.assertIn("published == SEAF_BRANCH_UPDATE_HEAD_CONFLICT && retry_cnt < 3", method)
        self.assertIn("g_set_error (error", method)
        self.assertIn("ret = -1;\n        goto out;", method)
        self.assertNotIn("if (!user) {\n        user = parent->creator_name;", method)

    def test_file_commit_retries_only_head_conflicts(self):
        method = function("server/repo-op.c", "gen_new_commit_guarded (",
                          "\nstatic int\ngen_new_commit (")
        self.assertIn("if (published != SEAF_BRANCH_UPDATE_HEAD_CONFLICT)", method)
        self.assertIn("if (published == SEAF_BRANCH_UPDATE_ACL_DENIED)", method)
        self.assertIn("MAX_RETRY_COUNT 10", method)

    def test_revert_retries_are_bounded_and_always_abort_on_error(self):
        method = function("server/repo-op.c",
                          "seaf_repo_manager_revert_on_server (",
                          "\nstatic void\nadd_deleted_entry")
        self.assertIn("published == SEAF_BRANCH_UPDATE_HEAD_CONFLICT && retry_cnt < 3", method)
        self.assertIn("CF_FILEOP_ABORTED (CF_OP_REVERT_REPO", method)
        self.assertIn("ret = -1;\n        goto out;", method)


if __name__ == "__main__":
    unittest.main()
