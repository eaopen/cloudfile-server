"""Cheap V2 regression guard: a directory ACL row never enrolls a whole library."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
REMOVED = re.compile(r'\b(?:checkLegacyLibrary|legacyLibraryAllowed|syncLegacyEntryDenied|cfACLUnscoped|cf_policy_managed_guard_required|cf_policy_check_legacy_library|cf_policy_check_legacy_access_reason|cf_policy_enroll_managed_library|cf_acl_legacy_entry_denied|CloudFileManagedLibraryGuard|CloudFileAllowLegacyManagedReads)\b')
RETIRED_SQL = re.compile(r'\b(?:FROM|JOIN|INTO|UPDATE|TABLE)\s+["`]?cf_(?:managed_library|managed_library_share|library_share_revision)\b', re.I)

class NoManagedLibraryBoundary(unittest.TestCase):
    def test_no_retired_go_server_c_seams(self):
        for directory in ('common', 'server', 'fileserver'):
            for file in (ROOT / directory).rglob('*'):
                if file.suffix not in ('.go', '.c', '.h') or file.name.endswith('_test.go'):
                    continue
                source = file.read_text(encoding='utf-8')
                with self.subTest(path=str(file.relative_to(ROOT))):
                    self.assertIsNone(REMOVED.search(source))
                    self.assertIsNone(RETIRED_SQL.search(source))

    def test_managed_enrollment_and_its_test_are_deleted(self):
        for name in ('fileserver/cloudfile_managed_library.go',
                     'fileserver/cloudfile_managed_library_test.go'):
            self.assertFalse((ROOT / name).exists(), name)

    def test_path_proof_precedes_publication(self):
        fileop = (ROOT / 'fileserver/fileop.go').read_text(encoding='utf-8')
        self.assertIn('cfACLFinal(ctx, trans, repoID, oldCommitID, newCommitID, aclActor)', fileop)
        self.assertIn('cfACLPath(repoID, info.User, filePath, "file", "read")', fileop)
        self.assertNotIn('checkLegacyLibrary(', fileop)
        branch = (ROOT / 'common/branch-mgr.c').read_text(encoding='utf-8')
        self.assertIn('cf_v2_acl_final_scoped(', branch)
        self.assertNotIn('cf_policy_enroll_managed_library(', branch)

    def test_whole_repo_protocol_is_a_capability_gate_only(self):
        source = (ROOT / 'fileserver/cf_v2_acl.go').read_text(encoding='utf-8')
        self.assertIn('func cfACLRequireRepoWideProof(', source)
        self.assertIn('func cfACLPath(', source)
        public = (ROOT / 'fileserver/cf_acl_public_scope.go').read_text(encoding='utf-8')
        self.assertIn('func cfACLPublicLinkScope(', public)
        self.assertIn("path=? OR ? LIKE CONCAT(path,'/%')", public)

if __name__ == '__main__':
    unittest.main()
