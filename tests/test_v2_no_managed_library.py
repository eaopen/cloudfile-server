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
        self.assertIn('cfACLFinal(ctx, trans, repoID, proof)', fileop)
        self.assertLess(fileop.index('cfACLPreflight(preCtx, repoID, oldCommitID, newCommitID, aclActor)'),
                        fileop.index('seafileDB.BeginTx(ctx, nil)'))
        self.assertIn('cfACLPath(repoID, info.User, filePath, "file", "read")', fileop)
        self.assertNotIn('checkLegacyLibrary(', fileop)
        branch = (ROOT / 'common/branch-mgr.c').read_text(encoding='utf-8')
        self.assertIn('cf_v2_acl_preflight_scoped(', branch)
        self.assertIn('cf_v2_acl_proof(trans,', branch)
        self.assertNotIn('cf_policy_enroll_managed_library(', branch)

    def test_c_final_acl_follows_branch_lock_and_head_validation(self):
        source = (ROOT / 'common/branch-mgr.c').read_text()
        commit = source.split('test_and_update_branch (', 1)[1]
        self.assertLess(commit.index('cf_v2_acl_preflight_scoped('),
                        commit.index('AND repo_id=? FOR UPDATE'))
        self.assertLess(commit.index('strcmp (old_commit_id, commit_id)'),
                        commit.index('cf_v2_acl_proof(trans,'))
        policy = (ROOT / 'common/cf-v2-acl.c').read_text()
        self.assertIn('g_strconcat(query, " FOR UPDATE", NULL)', policy)

    def test_whole_repo_protocol_is_a_capability_gate_only(self):
        source = (ROOT / 'fileserver/cf_v2_acl.go').read_text(encoding='utf-8')
        self.assertIn('func cfACLRequireRepoWideProof(', source)
        self.assertIn('func cfACLPath(', source)
        public = (ROOT / 'fileserver/cf_acl_public_scope.go').read_text(encoding='utf-8')
        self.assertIn('func cfACLPublicLinkScope(', public)
        self.assertIn("path=? OR ? LIKE CONCAT(path,'/%')", public)

    def test_directory_listing_uses_bounded_path_batch(self):
        adapter = (ROOT / 'common/cf-acl.c').read_text()
        self.assertIn('cf_v2_acl_check_json(seaf,payload,body)', adapter)
        self.assertIn('json_object_set_new(req,"partial",json_true())', adapter)
        self.assertIn('count<512', adapter)
        # The listing filter must use one bounded partial batch, not the
        # per-child cf_acl_apply seam. Scope the assertion to the filter: the
        # single-path sync gate still calls cf_acl_apply legitimately.
        listing = adapter.split('cf_acl_filter_dirents (', 1)[1]
        listing = listing.split('cf_acl_find_restricted_path', 1)[0]
        self.assertIn('filter_chunk(repo,dir,user,p,count,&visible)', listing)
        self.assertNotIn('cf_acl_apply(', listing)

    def test_upload_blks_is_path_qualified(self):
        source = (ROOT / 'fileserver/fileop.go').read_text()
        commit = source.split('func doUploadBlks(', 1)[1].split('func commitFileBlocks(', 1)[0]
        self.assertNotIn('cfACLRequireRepoWideProof(', commit)
        self.assertIn('isParentMatched(fsm.parentDir, parentDir)', commit)
        self.assertIn('cfACLPath(repoID, user, parentDir, "dir", "write")', commit)
        self.assertIn('cfACLPath(repoID, user, filepath.Join(parentDir, fileName), "file", "write")', commit)
        self.assertIn('commitFileBlocks(repoID, parentDir, fileName', commit)
        raw = source.split('func doUploadRawBlks(', 1)[1]
        self.assertIn('cfACLRequireRepoWideProof(fsm.repoID)', raw)

    def test_unused_directory_acl_feature_switch_is_not_restored(self):
        option = (ROOT / 'fileserver/option/option.go').read_text()
        self.assertNotIn('CloudFileDirACLEnabled', option)
        self.assertNotIn('dir_acl_enabled', option)
        policy = (ROOT / 'common/cf-acl.c').read_text()
        self.assertNotIn('dir_acl_enabled', policy)

    def test_native_links_check_target_and_v2_requires_go(self):
        native = (ROOT / 'server/http-tx-mgr.c').read_text()
        self.assertIn('cf_v2_acl_public_link_scope(seaf,repo_id,target,!file)', native)
        self.assertNotIn('cf_v2_acl_unscoped(seaf, seafile_share_link_info_get_repo_id(info))', native)
        policy = (ROOT / 'common/cf-v2-acl.c').read_text()
        self.assertIn('int cf_v2_acl_public_link_scope(', policy)
        self.assertIn("path=? OR ? LIKE CONCAT(path,'/%')", policy)
        session = (ROOT / 'server/seafile-session.c').read_text()
        self.assertIn('CloudFile V2 requires ENABLE_GO_FILESERVER=true', session)

    def test_hub_outage_is_not_reported_as_user_denial(self):
        go = (ROOT / 'fileserver/cf_v2_acl.go').read_text()
        self.assertIn('errCFACLUnavailable', go)
        self.assertIn('return cfACLPathFailure(err)', go)
        self.assertIn('http.StatusServiceUnavailable', go)
        tests = (ROOT / 'fileserver/cf_v2_acl_test.go').read_text()
        self.assertIn('TestCfACLAuthorityFailuresRemainDistinct', tests)
        native = (ROOT / 'server/http-tx-mgr.c').read_text()
        self.assertIn('*status=scope<0 ? 503 : 403;', native)
        policy = (ROOT / 'common/cf-v2-acl.c').read_text()
        self.assertIn('return intersects ? 1 : 0;', policy)

    def test_retired_fixtures_do_not_regain_authority(self):
        for name in ('tests/cf-dir-page', 'tests/cf-permission-many'):
            self.assertFalse((ROOT / name).exists(), name)
        contract = (ROOT / 'doc/server-architecture.md').read_text()
        self.assertIn('没有托管库', contract)
        self.assertIn('cf_edit_guard', contract)

    def test_retired_policy_and_ticket_engines_are_absent(self):
        forbidden = ('cf_policy_check_read', 'cf_policy_check_write', 'cf_acl_evaluate',
                     'cf_acl_resolve', 'with_barriers', 'cloudFileReadCB',
                     'seafile_cloudfile_issue_read_ticket', 'cf_lock_controlled_publish_enter')
        for base in ('common', 'server', 'include', 'fileserver', 'python'):
            for path in (ROOT / base).rglob('*'):
                if path.suffix not in ('.c', '.h', '.go', '.py') or path.name.endswith('_test.go'):
                    continue
                for token in forbidden:
                    self.assertNotIn(token, path.read_text(), str(path))
        self.assertNotIn('libcloudfile_acl', (ROOT / 'server/Makefile.am').read_text())
        native = (ROOT / 'server/web-accesstoken-mgr.c').read_text()
        self.assertIn('seaf_web_at_manager_get_scoped_download_token', native)

if __name__ == '__main__':
    unittest.main()
