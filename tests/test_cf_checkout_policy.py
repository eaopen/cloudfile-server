"""Cheap source contract for the OFF / OPTIONAL per-repository Checkout policy.

The native build is not part of routine CI, so keep the wiring assertions that a
compile/link would otherwise catch: the fence implementation must be compiled,
the C write barrier must consult the in-memory repository policy before SQL, and
an OPTIONAL library must fail closed when its Guard schema is absent.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CheckoutPolicyContract(unittest.TestCase):
    def test_fence_implementation_is_part_of_the_native_build(self):
        makefile = (ROOT / 'server/Makefile.am').read_text()
        self.assertIn('../common/cf-v2-edit-fence.c', makefile)
        self.assertIn('../common/cf-lock.c', makefile)

    def test_write_barrier_checks_policy_before_guard_sql(self):
        source = (ROOT / 'common/cf-lock.c').read_text()
        start = source.index('check_path (const char *repo_id')
        body = source[start:source.index('\n}\n', start)]
        self.assertIn('cf_v2_edit_repo_checkout', body)
        self.assertLess(body.index('cf_v2_edit_repo_checkout'),
                        body.index('cf_ext_database'))

    def test_optional_without_guard_fails_closed(self):
        source = (ROOT / 'common/cf-v2-edit-fence.c').read_text()
        self.assertIn('policy = cf_v2_edit_repo_checkout', source)
        # guard_available() must not report an absent schema as usable.
        self.assertIn('return guard_schema_state == 2 ? 1 : -1;', source)

    def test_policy_parser_rejects_noncanonical_and_duplicate_uuids(self):
        source = (ROOT / 'common/cf-v2-edit-fence.c').read_text()
        self.assertIn('canonical_repo_id', source)
        self.assertIn('g_hash_table_contains(checkout_repo_ids, id)', source)
        self.assertIn('checkout_policy_state == 2', source)


    def test_native_branch_cas_calls_the_guard_fence_before_update(self):
        source = (ROOT / 'common/branch-mgr.c').read_text()
        start = source.index('test_and_update_branch (SeafBranchManager *mgr')
        body = source[start:source.index('\n}\n', start)]
        self.assertIn('cf_v2_edit_check_unscoped_branch', body)
        self.assertIn('cf-v2-edit-fence.h', source)
        self.assertLess(body.index('cf_v2_edit_check_unscoped_branch'),
                        body.index('UPDATE Branch'))
        self.assertLess(body.index('cf_v2_acl_proof(trans,'),
                        body.index('cf_v2_edit_check_unscoped_branch'))
        # A guard denial is permanent: same non-retryable class as ACL denial.
        self.assertIn('guard_result < 0', body)

    def test_optional_status_never_reports_unknown_guard_as_unlocked(self):
        source = (ROOT / 'fileserver/cf_v2_edit_agent.go').read_text()
        # The API-level early refusal covers writes and status (only read is exempt).
        self.assertIn('selected && !option.V2GuardAvailable && route != "read"', source)
        start = source.index('func cfEditStatus(')
        body = source[start:source.index('\n}\n', start)]
        self.assertIn('GUARD_UNAVAILABLE', body)

if __name__ == '__main__':
    unittest.main()
