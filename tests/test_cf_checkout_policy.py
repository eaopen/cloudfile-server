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


if __name__ == '__main__':
    unittest.main()
