"""What the native side asks the Hub when an account has no library grant.

The seam itself (who may call it, and only after a denial) is proven in
tests/cf-permission-many; this file proves the exchange: the request names the
exact path the caller asked about, and only an explicit r/rw level is ever
turned into a permission. Everything else stays "no qualification", because a
wrong guess here hands a share-only account a write it was never granted.
"""
import ctypes
import json
import os
import unittest

lib = ctypes.CDLL(os.environ['CF_DIR_SHARE_LIBRARY'])
lib.cf_v2_share_grant.argtypes = [ctypes.c_char_p] * 3 + [ctypes.POINTER(ctypes.c_int)]
lib.cf_v2_share_grant.restype = ctypes.c_void_p
lib.fixture_reply.argtypes = [ctypes.c_int, ctypes.c_char_p]
lib.fixture_payload.restype = ctypes.c_char_p
lib.fixture_free.argtypes = [ctypes.c_void_p]
lib.fixture_registration_context.argtypes = [ctypes.c_int] * 4
lib.fixture_registrations.restype = ctypes.c_int
lib.cf_v2_share_init.argtypes = []
lib.cf_v2_share_init.restype = None
REPO = '11111111-1111-4111-8111-111111111111'
ALLOW = json.dumps(dict(allowed=True, contract='cf-acl-v1', permissions=['rw']))


def grant(path='/shared/team', user='shared-user@example.test', repo=REPO):
    unavailable = ctypes.c_int(0)
    pointer = lib.cf_v2_share_grant(repo.encode(), path.encode(), user.encode(),
                                    ctypes.byref(unavailable))
    value = None
    if pointer:
        try:
            value = ctypes.string_at(pointer).decode()
        finally:
            lib.fixture_free(pointer)
    return value, bool(unavailable.value)


def reply(status, body):
    lib.fixture_reply(status, body.encode() if body is not None else None)


class DirectoryShareTests(unittest.TestCase):
    def setUp(self):
        lib.fixture_reset()

    def reply(self, permissions=None, status=200, **extra):
        body = dict(allowed=True, contract='cf-acl-v1', **extra)
        if permissions is not None:
            body['permissions'] = permissions
        reply(status, json.dumps(body))

    def test_qualified_root_returns_the_published_level(self):
        self.reply(['rw'])
        self.assertEqual(grant(), ('rw', False))
        self.reply(['r'])
        self.assertEqual(grant(), ('r', False))

    def test_request_names_the_exact_path_and_user(self):
        # The Hub decides per path. A single wrong field would make the answer
        # about a different directory, which is the whole failure mode this
        # seam replaces.
        self.reply(['rw'])
        grant('/shared/team/design')
        self.assertEqual(json.loads(lib.fixture_payload()), dict(
            username='shared-user@example.test', repo_id=REPO,
            checks=[dict(path='/shared/team/design', kind='dir', action='read')]))

    def test_a_trailing_slash_is_the_same_directory(self):
        # Callers hand over whatever the upstream RPC received; the Hub rejects
        # the trailing-slash spelling with 400, which would qualify a path for
        # some entry points and not others.
        self.reply(['rw'])
        self.assertEqual(grant('/shared/team/'), ('rw', False))
        self.assertEqual(json.loads(lib.fixture_payload())['checks'][0]['path'],
                         '/shared/team')

    def test_the_library_root_is_never_a_share_root(self):
        self.reply(['rw'])
        self.assertEqual(grant('/'), (None, False))
        self.assertEqual(lib.fixture_calls(), 0)

    def test_a_relative_path_never_reaches_the_authority(self):
        self.reply(['rw'])
        for path in ('', 'shared/team'):
            with self.subTest(path=path):
                self.assertEqual(grant(path), (None, False))
        self.assertEqual(lib.fixture_calls(), 0)
        # Dot segments are still the Hub's to reject: a second canonicalizer
        # here would be a second source of truth for what a path means. The
        # fake authority accepts everything, so this asserts what was asked,
        # not what came back.
        grant('/shared/../etc')
        self.assertEqual(json.loads(lib.fixture_payload())['checks'][0]['path'],
                         '/shared/../etc')

    def test_denial_is_not_an_outage(self):
        reply(403, json.dumps(dict(code='ACL_DENIED')))
        self.assertEqual(grant(), (None, False))

    def test_outage_is_reported_as_unavailable(self):
        for status in (500, 502, 503, -1):
            with self.subTest(status=status):
                lib.fixture_reset()
                reply(status, '{"code":"ACL_UNAVAILABLE"}')
                self.assertEqual(grant(), (None, True))

    def test_a_verdict_without_a_level_is_not_a_grant(self):
        # An older Hub answers allowed/contract only. Assuming rw would hand out
        # a write; assuming r would break uploads with a misleading 403. The
        # only safe reading is "this authority cannot qualify".
        for extra in ({}, dict(permissions=[]), dict(permissions=['rw', 'r']),
                      dict(permissions=[None]), dict(permissions=['admin']),
                      dict(permissions=['invisible']), dict(permissions='rw')):
            with self.subTest(extra=extra):
                self.reply(**extra)
                self.assertEqual(grant(), (None, False))

    def test_contract_and_verdict_are_both_required(self):
        for body in (dict(allowed=False, contract='cf-acl-v1', permissions=['rw']),
                     dict(allowed=True, contract='v1', permissions=['rw']),
                     dict(allowed='true', contract='cf-acl-v1', permissions=['rw'])):
            with self.subTest(body=body):
                reply(200, json.dumps(body))
                self.assertEqual(grant(), (None, False))
        reply(200, 'not json')
        self.assertEqual(grant(), (None, False))


class RegistrationTests(unittest.TestCase):
    """The three-table V2 install must register without retired tables or flags."""

    def setUp(self):
        lib.fixture_reset()

    def registration(self, *, schema, db_kind=1, channel=1, fault=0):
        lib.fixture_registration_context(schema, db_kind, channel, fault)
        lib.cf_v2_share_init()
        return lib.fixture_registrations()

    def test_standard_v2_three_tables_register_the_share_grant(self):
        self.assertEqual(self.registration(schema=1), 1)

    def test_missing_schema_or_non_mysql_keeps_ce_denial(self):
        self.assertEqual(self.registration(schema=0), 0)
        self.assertEqual(self.registration(schema=1, db_kind=2), 0)

    def test_unreadable_schema_or_missing_internal_channel_never_registers(self):
        self.assertEqual(self.registration(schema=1, fault=1), 0)
        self.assertEqual(self.registration(schema=1, channel=0), 0)


if __name__ == '__main__':
    unittest.main()
