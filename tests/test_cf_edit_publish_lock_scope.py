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
    def _publish(self):
        return SOURCE.split("func cfEditPublish(", 1)[1].split("\nfunc ", 1)[0]

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
