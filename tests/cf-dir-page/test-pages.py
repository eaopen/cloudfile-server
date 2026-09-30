"""Native scan/RPC regression cases shared with Hub; no running service needed."""
import ctypes
import json
import os
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NativePageFixture:
    def __init__(self):
        self.lib = ctypes.CDLL(os.environ['CF_DIR_PAGE_TEST_LIBRARY'])
        for name in ('fixture_reset', 'fixture_set_revision', 'fixture_deny_parent', 'fixture_fail_read', 'fixture_invalid'):
            fn = getattr(self.lib, name)
            fn.argtypes = [ctypes.c_char_p if name.endswith('revision') else ctypes.c_int]
            fn.restype = None
        self.lib.fixture_hide.argtypes = [ctypes.c_int, ctypes.c_int]
        self.lib.fixture_hide.restype = None
        self.lib.fixture_page.argtypes = [ctypes.c_char_p]
        self.lib.fixture_page.restype = ctypes.c_void_p
        self.lib.fixture_free.argtypes = [ctypes.c_void_p]
        self.lib.fixture_free.restype = None
        self.lib.fixture_legacy_count.restype = ctypes.c_int

    def raw(self, request):
        pointer = self.lib.fixture_page(request.encode())
        if not pointer: raise RuntimeError('Native returned NULL without an error')
        try:
            result = ctypes.string_at(pointer).decode()
            if 'rpc_error' in json.loads(result): raise RuntimeError(result)
            return result
        finally:
            self.lib.fixture_free(pointer)

    def page(self, start=0, limit=3, revision='a' * 40, **changes):
        request = dict(repo_id='11111111-1111-4111-8111-111111111111', path='/',
                       user='user', dir_revision=revision, start=start, limit=limit)
        return json.loads(self.raw(json.dumps({**request, **changes})))


class NativePagesTest(unittest.TestCase):
    def setUp(self):
        self.native = NativePageFixture()
        self.native.lib.fixture_reset(10)

    def test_shared_windows_and_complete_traversal(self):
        cases = json.loads((ROOT.parent / 'cloudfile-docker/docs/directory-pagination-cases.json').read_text())
        for case in cases:
            with self.subTest(case=case['name']):
                self.native.lib.fixture_reset(case['total'])
                for index in case['hidden']: self.native.lib.fixture_hide(index, 1)
                seen = []
                for expected in case['pages']:
                    page = self.native.page(expected['start'], case['limit'])
                    self.assertEqual(page['dir_revision'], 'a' * 40)
                    for field in ('scanned_count', 'scan_exhausted', 'next_scan_position'):
                        self.assertEqual(page[field], expected[field])
                    ids = [int(entry['obj_name'][1:]) for entry in page['visible_items']]
                    self.assertEqual(ids, expected['visible'])
                    self.assertEqual(page['visible_count'], len(ids))
                    seen.extend(ids)
                self.assertEqual(len(seen), len(set(seen)))
                self.assertEqual(seen, [i for i in range(case.get('initial_start', 0), case['total']) if i not in case['hidden']])

    def test_revision_change_is_not_a_successful_page(self):
        self.native.page()
        self.native.lib.fixture_set_revision(b'b' * 40)
        self.assertEqual(self.native.page(3), {'error': 'DIR_REVISION_CHANGED'})
        self.assertEqual(self.native.page(0, revision='b' * 40)['dir_revision'], 'b' * 40)

    def test_current_permissions_are_reloaded_between_pages(self):
        self.assertEqual(len(self.native.page()['visible_items']), 3)
        for i in (3, 4, 5): self.native.lib.fixture_hide(i, 1)
        page = self.native.page(3)
        self.assertEqual(page['visible_items'], [])
        self.assertFalse(page['scan_exhausted'])
        self.assertEqual(page['next_scan_position'], 6)
        self.native.lib.fixture_hide(3, 0)
        self.assertEqual(self.native.page(3)['visible_count'], 1)
        self.native.lib.fixture_deny_parent(1)
        with self.assertRaises(RuntimeError): self.native.page(6)

    def test_invalid_object_still_advances_raw_cursor(self):
        self.native.lib.fixture_invalid(1)
        page = self.native.page()
        self.assertEqual(page['scanned_count'], 3)
        self.assertEqual(page['visible_count'], 2)
        self.assertEqual(page['next_scan_position'], 3)

    def test_read_failure_does_not_become_exhaustion(self):
        self.native.lib.fixture_fail_read(1)
        with self.assertRaises(RuntimeError): self.native.page()

    def test_invalid_requests_fail_before_scanning(self):
        for changes in ({'start': -1}, {'start': 2147483647}, {'limit': 0}, {'limit': 501},
                        {'limit': True}, {'start': 0.5}, {'repo_id': 'bad'}, {'dir_revision': 'bad'},
                        {'path': ''}, {'user': ''}):
            with self.subTest(changes=changes), self.assertRaises(RuntimeError): self.native.page(**changes)

    def test_out_of_range_offset_is_terminal(self):
        page = self.native.page(1000)
        self.assertEqual(page['scanned_count'], 0)
        self.assertTrue(page['scan_exhausted'])
        self.assertIsNone(page['next_scan_position'])

    def test_dirent_wire_fields_and_legacy_unlimited_listing(self):
        entry = self.native.page()['visible_items'][0]
        self.assertEqual(entry['permission'], 'rw')
        self.assertEqual(entry['mtime'], 9000000000000)
        self.assertEqual(entry['size'], 8000000000000)
        self.assertEqual(entry['is_locked'], False)
        self.assertIsNone(entry['lock_owner'])
        self.assertEqual(self.native.lib.fixture_legacy_count(), 10)


if __name__ == '__main__': unittest.main()
