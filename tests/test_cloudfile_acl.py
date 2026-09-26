"""Compile and exercise the actual C policy core, not a Python reimplementation.

This is a policy-unit test. It does not prove REST/fileserver enforcement.
"""
import ctypes as c
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEXTS = c.POINTER(c.c_char_p)


class Rule(c.Structure):
    _fields_ = [("path", c.c_char_p), ("subject_id", c.c_char_p),
                ("subject_type", c.c_int), ("permission", c.c_int),
                ("inherit", c.c_int), ("kind", c.c_int)]


class Context(c.Structure):
    _fields_ = [("user_id", c.c_char_p), ("departments", TEXTS),
                ("department_count", c.c_size_t), ("groups", TEXTS),
                ("group_count", c.c_size_t), ("ce_permission", c.c_int),
                ("ready", c.c_int), ("active", c.c_int),
                ("hard_readonly", c.c_int), ("barrier_active", c.c_int)]


class Result(c.Structure):
    _fields_ = [("visible", c.c_int), ("read", c.c_int), ("write", c.c_int)]


class CloudfileAclTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="cf-acl-test-")
        cls.addClassCleanup(cls.temp.cleanup)
        output = Path(cls.temp.name) / "acl.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-shared", "-fPIC", str(ROOT / "server/cloudfile-acl.c"),
                        "-o", str(output)], check=True)
        cls.library = c.CDLL(str(output))
        cls.library.cf_acl_evaluate.argtypes = [c.POINTER(Context), c.c_char_p, c.c_int,
                                               c.POINTER(Rule), c.c_size_t, c.POINTER(Result)]
        cls.library.cf_acl_evaluate.restype = c.c_int
        cls.vectors = json.loads((ROOT.parent / "eap-cloudfile/contracts/acceptance-vectors.json").read_text())

    def evaluate(self, changes):
        values = {**self.vectors["acl_defaults"], **changes}
        encode = lambda value: value.encode("utf-8")
        permissions = {None: -1, "invisible": 0, "none": 1, "r": 2, "rw": 3}
        kinds = {"dir": 0, "file": 1}
        groups = (c.c_char_p * len(values["groups"]))(*map(encode, values["groups"]))
        depts = (c.c_char_p * len(values["departments"]))(*map(encode, values["departments"]))
        ctx = Context(encode(values["userId"]), depts, len(depts), groups, len(groups),
                      permissions[values["ce_permission"]], values["context_ready"],
                      values["account_active"], values["repo_hard_readonly"], values["barrier_active"])
        rules = (Rule * len(values["rules"]))(*[
            Rule(encode(r["path"]), encode(r["subject_id"]),
                 {"user": 3, "dept": 2, "group": 1}[r["subject_type"]],
                 permissions[r["permission"]], r["inherit"], kinds[r.get("kind", "dir")])
            for r in values["rules"]])
        result = Result(1, 1, 1)
        status = self.library.cf_acl_evaluate(c.byref(ctx), encode(values["target"]["path"]),
                                              kinds[values["target"]["kind"]], rules, len(rules), c.byref(result))
        return status, {name: bool(getattr(result, name)) for name in ("visible", "read", "write")}

    def test_shared_acceptance_vectors(self):
        for vector in self.vectors["acl_cases"]:
            with self.subTest(case=vector["id"]):
                status, result = self.evaluate(vector["input"])
                self.assertEqual(status, 0)
                self.assertEqual(result, vector["expected"])

    def test_rule_order_cannot_change_result(self):
        for vector in self.vectors["acl_cases"]:
            changes = vector["input"]
            if "rules" in changes:
                self.assertEqual(self.evaluate(changes), self.evaluate({**changes, "rules": list(reversed(changes["rules"]))}))

    def test_non_boolean_native_context_flags_fail_closed(self):
        # Do not treat malformed SQL/RPC integer flags as authenticated truth.
        for field in ("context_ready", "account_active", "repo_hard_readonly", "barrier_active"):
            for invalid in (-1, 2, 2147483647):
                with self.subTest(field=field, invalid=invalid):
                    status, result = self.evaluate({field: invalid, "ce_permission": "rw"})
                    self.assertEqual(status, -1)
                    self.assertEqual(result, {"visible": False, "read": False, "write": False})

    def test_file_deny_and_segment_boundaries(self):
        rule = {"path": "/parts/model.prt", "kind": "file", "subject_type": "user",
                "subject_id": "u1", "permission": "none", "inherit": True}
        self.assertEqual(self.evaluate({"target": {"path": rule["path"], "kind": "file"}, "rules": [rule]}),
                         (0, {"visible": True, "read": False, "write": False}))
        rule = {**rule, "path": "/part", "kind": "dir", "permission": "invisible"}
        self.assertEqual(self.evaluate({"rules": [rule]})[1], {"visible": True, "read": True, "write": False})

    def test_invalid_path_file_grant_and_duplicate_conflict_fail_closed(self):
        rule = {"path": "/parts", "subject_type": "user", "subject_id": "u1",
                "permission": "r", "inherit": True}
        for changes in (
            {"target": {"path": "/parts/../secret", "kind": "file"}},
            {"rules": [{**rule, "kind": "file"}]},
            {"rules": [rule, {**rule, "permission": "rw"}]},
        ):
            status, result = self.evaluate(changes)
            self.assertEqual(status, -1)
            self.assertEqual(result, {"visible": False, "read": False, "write": False})


if __name__ == "__main__":
    unittest.main()
