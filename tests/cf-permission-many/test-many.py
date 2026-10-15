"""Strict native envelope and actual scalar/provider/boundary parity."""
import ctypes
import json
import os
import unittest

lib = ctypes.CDLL(os.environ['CF_PERMISSION_MANY_LIBRARY'])
for name, args in [('fixture_many', [ctypes.c_char_p]), ('fixture_scalar', [ctypes.c_char_p] * 3)]:
    function = getattr(lib, name)
    function.argtypes, function.restype = args, ctypes.c_void_p
lib.fixture_free.argtypes = [ctypes.c_void_p]
REPO = '11111111-1111-4111-8111-111111111111'


def call(name, *args):
    pointer = getattr(lib, name)(*(arg.encode() for arg in args))
    if not pointer: return None
    try: return ctypes.string_at(pointer).decode()
    finally: lib.fixture_free(pointer)


def request(paths):
    return dict(version=1, repo_id=REPO, user='alice', paths=paths)


class ManyTests(unittest.TestCase):
    def setUp(self): lib.fixture_reset()

    def test_scalar_equivalence(self):
        # 2026-10-15 v0.9-only: the ACL provider is the managed-library
        # boundary. allow: every path keeps the native permission except the
        # hook provider's own denial; managed: every path is refused.
        for mode in ('allow', 'managed', 'repo_deny'):
            with self.subTest(mode=mode):
                lib.fixture_reset()
                if mode == 'managed': lib.fixture_manage(1)
                if mode == 'repo_deny': lib.fixture_deny_repo(1)
                paths = ['/', '/a', '/a/file', '/a/file', '/b/file', '/hook']
                scalar = [call('fixture_scalar', REPO, p, 'alice') for p in paths]
                if mode == 'repo_deny':
                    for value in scalar: self.assertIsNone(value)
                    continue
                expected = [None if mode == 'managed' or p == '/hook' else 'rw'
                            for p in paths]
                self.assertEqual(scalar, expected)
                result = json.loads(call('fixture_many', json.dumps(request(paths))))
                self.assertEqual([i['permission'] for i in result['items']], expected)
                self.assertEqual([i['path'] for i in result['items']], paths)
                self.assertEqual(lib.fixture_count(0), len(paths) * 2)

    def test_managed_boundary_is_checked_per_path(self):
        lib.fixture_manage(1)
        result = json.loads(call('fixture_many', json.dumps(request(['/a', '/b']))))
        self.assertEqual([i['permission'] for i in result['items']], [None, None])
        self.assertEqual(lib.fixture_count(1), 2)

    def test_normalized_unicode_root_and_duplicates(self):
        paths = ['/', '/中文/', '/中文', '/percent%20literal']
        result = json.loads(call('fixture_many', json.dumps(request(paths))))
        self.assertEqual([i['path'] for i in result['items']], ['/', '/中文', '/中文', '/percent%20literal'])
        self.assertEqual(lib.fixture_count(0), 4)

    def test_request_validation_precedes_every_scalar_call(self):
        invalid = [[], ['/'] * 51, [None], ['relative'], ['//a'], ['/./a'], ['/a/../b'],
                   ['/a\x00b'], ['/' + '中' * 1366], ['/' + 'a/' * 130], ['/ok', '/bad//path']]
        raws = [json.dumps(request(paths)) for paths in invalid]
        for field, value in [('repo_id', 'invalid'), ('user', ''), ('user', 'x' * 256), ('version', True), ('extra', 1)]:
            value_dict = request(['/']); value_dict[field] = value
            raws.append(json.dumps(value_dict))
        raws += ['{"version":1,"version":1}', 'x' * 65537,
                 json.dumps(request(['/' + '中' * 1200] * 50), ensure_ascii=False)]
        for raw in raws:
            with self.subTest(raw=raw[:80]):
                self.assertEqual(json.loads(call('fixture_many', raw)), {'rpc_error': True})
                self.assertEqual(lib.fixture_count(0), 0)

    def test_provider_error_discards_partial_batch(self):
        lib.fixture_fail_at(2)
        self.assertEqual(json.loads(call('fixture_many', json.dumps(request(['/a', '/b', '/c'])))), {'rpc_error': True})
        self.assertEqual(lib.fixture_count(0), 2)

    def test_elapsed_native_deadline_discards_partial_batch(self):
        lib.fixture_timeout_at(2)
        self.assertEqual(json.loads(call('fixture_many', json.dumps(request(['/a', '/b', '/c'])))), {'rpc_error': True})
        self.assertEqual(lib.fixture_count(0), 2)

    def test_counters_do_not_confuse_transport_with_engine(self):
        result = json.loads(call('fixture_many', json.dumps(request(['/' + str(i) for i in range(50)]))))
        self.assertEqual(len(result['items']), 50)
        self.assertEqual([lib.fixture_count(i) for i in range(3)], [50] * 3)

if __name__ == '__main__': unittest.main()
