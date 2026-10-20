"""Native permission for an account that holds only a directory share.

The reported P0 (2026-10-20) was that a user with no library permission who was
granted rw on a shared directory could browse and download but never upload,
mkdir or rename: EAP allowed the write, then the native upload link / mkdir /
rename / delete gate asked seafile_check_permission_by_path, got nothing from
repo-perm.c, and stopped. These cases pin the native half of the fix -- the
qualification is consulted, and it is consulted *only* when the library gate
denied rather than failed.
"""
import ctypes
import json
import os
import unittest

lib = ctypes.CDLL(os.environ['CF_PERMISSION_MANY_LIBRARY'])
lib.fixture_scalar.argtypes = [ctypes.c_char_p] * 3
lib.fixture_scalar.restype = ctypes.c_void_p
lib.fixture_share_level.argtypes = [ctypes.c_char_p]
lib.fixture_free.argtypes = [ctypes.c_void_p]
REPO = '11111111-1111-4111-8111-111111111111'


def scalar(path='/shared/team'):
    pointer = lib.fixture_scalar(REPO.encode(), path.encode(), b'alice')
    if not pointer:
        return None
    try:
        return ctypes.string_at(pointer).decode()
    finally:
        lib.fixture_free(pointer)


class ShareQualificationTests(unittest.TestCase):
    def setUp(self):
        lib.fixture_reset()

    def test_share_root_replaces_a_missing_library_grant(self):
        lib.fixture_deny_repo(1)
        lib.fixture_share_level(b'rw')
        self.assertEqual(scalar(), 'rw')
        # The tightening chain starts from a native grant; there is none, so it
        # must not have been entered at all (index 2 is its hook).
        self.assertEqual(lib.fixture_count(2), 0)
        self.assertEqual(lib.fixture_count(3), 1)

    def test_read_only_share_never_becomes_a_write(self):
        lib.fixture_deny_repo(1)
        lib.fixture_share_level(b'r')
        self.assertEqual(scalar(), 'r')

    def test_unqualified_path_still_denies_without_an_error(self):
        # The normal case: no grant anywhere. A denial stays a denial, so the
        # Hub keeps answering 403 instead of inventing a 503.
        lib.fixture_deny_repo(1)
        self.assertIsNone(scalar())
        self.assertEqual(lib.fixture_count(3), 1)

    def test_unreadable_qualification_is_an_outage_not_a_denial(self):
        lib.fixture_deny_repo(1)
        lib.fixture_share_unavailable(1)
        self.assertEqual(json.loads(scalar()), {'rpc_error': True})

    def test_native_member_never_asks_the_share_question(self):
        # A member keeps exactly the old path: native permission, then the
        # tightening chain. The qualification is a fallback, not a second vote.
        lib.fixture_share_level(b'rw')
        self.assertEqual(scalar(), 'rw')
        self.assertEqual(lib.fixture_count(3), 0)
        self.assertEqual(lib.fixture_count(2), 1)

    def test_a_failing_library_gate_is_propagated_not_requalified(self):
        lib.fixture_fail_at(1)
        lib.fixture_share_level(b'rw')
        self.assertEqual(json.loads(scalar()), {'rpc_error': True})
        self.assertEqual(lib.fixture_count(3), 0)

    def test_managed_boundary_denial_is_not_reopened_by_a_share(self):
        # Native rw plus the managed-library boundary: the legacy entry refuses
        # because it cannot adjudicate that library. A share qualification must
        # not turn that refusal into a grant.
        lib.fixture_manage(1)
        lib.fixture_share_level(b'rw')
        self.assertIsNone(scalar())
        self.assertEqual(lib.fixture_count(3), 0)


if __name__ == '__main__':
    unittest.main()
