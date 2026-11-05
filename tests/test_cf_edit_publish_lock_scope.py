"""Source contract: cf-edit publication must not call Hub ACL under Branch(master).

Issue #43: cf-edit publish used to acquire Branch(master) FOR UPDATE and only
then call Hub /cloudfile/acl/check/ inside that transaction. Phase A now takes
the bounded ACL row/revision proof and asks Hub once BEFORE BeginTx; Phase B
re-reads the same proof under the lock and never performs remote HTTP or a
commit diff. This is a cheap ordering check; real concurrency proof needs
MariaDB and a Hub instance.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "fileserver/cf_v2_edit_agent.go").read_text(encoding="utf-8")


class CFEditPublishLockScope(unittest.TestCase):
    def _function(self, name):
        return SOURCE.split(f"func {name}(", 1)[1].split("\nfunc ", 1)[0]

    def _publish(self):
        # Transport preparation moved to cfEditPublish; both full upload and CDC
        # must reach the same commit helper, where the lock invariants still live.
        return self._function("cfEditCommit")

    def test_both_transports_use_the_shared_commit_fence(self):
        full = self._function("cfEditPublish")
        cdc = (ROOT / "fileserver/cf_v2_edit_cdc.go").read_text(encoding="utf-8")
        self.assertIn("cfEditCommit(w, r, c, repo, action, base, proof, fileID, size, gcID)", full)
        self.assertIn("cfEditCommit(w, r, c, repo, action, m.Base, proof, f.FileID, m.Size, gc)", cdc)
        self.assertNotIn("seafileDB.BeginTx(", full)
        self.assertNotIn("seafileDB.BeginTx(", cdc)

    def test_full_upload_captures_gc_before_indexing_and_locks_before_branch(self):
        full = self._function("cfEditPublish")
        self.assertLess(full.index("repomgr.GetCurrentGCID("), full.index("indexBlocks("))
        commit = self._publish()
        self.assertLess(commit.index("SELECT gc_id FROM GCID"), commit.index("cfEditHead("))
        self.assertIn("current.String != gcID", commit)
        self.assertNotIn("gcID != nil", commit)

    def test_acl_preflight_runs_before_the_branch_transaction(self):
        body = self._publish()
        self.assertLess(body.index("cfEditPermissionPreflight("),
                        body.index("seafileDB.BeginTx("))

    def test_no_hub_authorization_under_the_branch_transaction(self):
        body = self._publish()
        self.assertNotIn("cfACLAuthorize(", body)
        self.assertNotIn("cfEditCurrentPermission(", SOURCE)
        self.assertGreater(body.index("cfACLFinal("),
                           body.index("seafileDB.BeginTx("))

    def test_preflight_keeps_one_hub_write_decision_and_the_acl_proof(self):
        helper = SOURCE.split("func cfEditPermissionPreflight(", 1)[1].split("\nfunc ", 1)[0]
        self.assertIn("cfACLCurrentProof(ctx, seafileDB, repoID, false)", helper)
        self.assertIn("cfACLAuthorize(ctx, repoID, user", helper)
        self.assertIn("errCFACL", helper)


if __name__ == "__main__":
    unittest.main()
