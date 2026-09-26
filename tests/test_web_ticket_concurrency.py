"""Actual native RPC ticket races on an explicitly isolated test server.

No mock token store. Does not prove fileserver ACL/path or cleanup-timer races.
Run with unittest only after the isolated native server is prepared.
"""
from concurrent.futures import ThreadPoolExecutor
import os
from threading import Barrier
import unittest
from uuid import uuid4


@unittest.skipUnless(os.environ.get("CF_TEST_SERVER_ISOLATED") == "1"
    and os.environ.get("CF_TEST_SERVER_PIPE"), "requires explicitly isolated native RPC server")
class WebTicketConcurrencyTests(unittest.TestCase):
    def client(self):
        from seafile.rpcclient import SeafServerThreadedRpcClient
        return SeafServerThreadedRpcClient(os.environ["CF_TEST_SERVER_PIPE"])

    def setUp(self):
        self.rpc = self.client()
        self.owner = "cf-ticket-fixture-" + uuid4().hex + "@invalid.test"
        self.repo_id = self.rpc.create_repo("cf-ticket-fixture-" + uuid4().hex,
            "isolated ticket race", self.owner, None, 2, None, None)
        self.assertTrue(self.repo_id)
        self.addCleanup(self.rpc.remove_repo, self.repo_id)

    def query_race(self, token, *, count=8):
        barrier = Barrier(count)
        def query(_):
            # Do not serialize the race behind a shared Python RPC transport.
            client = self.client()
            barrier.wait(timeout=10)
            return client.seafile_web_query_access_token(token)
        with ThreadPoolExecutor(max_workers=count) as executor:
            return list(executor.map(query, range(count)))

    def token(self, *, once):
        return self.rpc.seafile_web_get_access_token(self.repo_id, "0" * 40,
            "download", self.owner, once)

    def test_single_use_has_exactly_one_winner(self):
        for _ in range(25):
            token = self.token(once=1)
            outcomes = self.query_race(token)
            self.assertEqual(sum(value is not None for value in outcomes), 1)
            self.assertIsNone(self.rpc.seafile_web_query_access_token(token))

    def test_reusable_ticket_survives_concurrent_queries(self):
        token = self.token(once=0)
        outcomes = self.query_race(token)
        self.assertTrue(all(value is not None for value in outcomes))
        self.assertIsNotNone(self.rpc.seafile_web_query_access_token(token))

    def test_missing_ticket_does_not_block_following_query(self):
        self.assertIsNone(self.rpc.seafile_web_query_access_token(str(uuid4())))
        self.assertIsNotNone(self.rpc.seafile_web_query_access_token(self.token(once=1)))
