"""Exercise scoped qualification against the compiled production policy core."""
import ctypes as c
import json
from pathlib import Path
import unittest

import test_cloudfile_acl as legacy
from test_cloudfile_acl import Context, Rule, Result, ROOT


class Share(c.Structure):
    _fields_ = [("path", c.c_char_p), ("subject_id", c.c_char_p), ("subject_type", c.c_int)]


class ScopedAclTest(legacy.CloudfileAclTest):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.scoped = cls.library.cf_acl_evaluate_scoped
        cls.scoped.argtypes = [c.POINTER(Context), c.c_char_p, c.c_int,
            c.POINTER(Rule), c.c_size_t, c.POINTER(Share), c.c_size_t, c.POINTER(Result)]
        cls.scoped.restype = c.c_int
        cls.scoped_vectors = json.loads((ROOT.parent / "cloudfile-docker/docs/directory-access-v09-cases.json").read_text())

    def scoped_result(self, changes):
        v = {**self.scoped_vectors['defaults'], **changes}
        perm = {None: 1, 'invisible': 0, 'none': 1, 'r': 2, 'rw': 3}
        types = {'user': 3, 'dept': 2, 'group': 1}
        kinds = {'dir': 0, 'file': 1}
        deps = (c.c_char_p * len(v['departments']))(*(x.encode() for x in v['departments']))
        groups = (c.c_char_p * len(v['groups']))(*(x.encode() for x in v['groups']))
        ctx = Context(v['userId'].encode(), deps, len(deps), groups, len(groups), perm[v['ce_permission']],
            v['context_ready'], v['account_active'], v['repo_hard_readonly'], v['barrier_active'])
        rules = (Rule * len(v['rules']))(*[Rule(r['path'].encode(), r['subject_id'].encode(),
            types[r['subject_type']], perm[r['permission']], r['inherit'], kinds[r.get('kind', 'dir')]) for r in v['rules']])
        shares = (Share * len(v['shares']))(*[Share(s['path'].encode(), s['subject_id'].encode(), types[s['subject_type']]) for s in v['shares']])
        out = Result(1, 1, 1)
        status = self.scoped(c.byref(ctx), v['target']['path'].encode(), kinds[v['target']['kind']],
            rules, len(rules), shares, len(shares), c.byref(out))
        return status, {name: bool(getattr(out, name)) for name in ('visible', 'read', 'write')}

    def test_scoped_vectors(self):
        for case in self.scoped_vectors['cases']:
            with self.subTest(case=case['id']):
                self.assertEqual(self.scoped_result(case['input']), (0, case['expected']))

    def test_invalid_share_paths_fail_closed(self):
        for path in ('/', '/a/../team', '/team/', '/team//x'):
            with self.subTest(path=path):
                self.assertEqual(self.scoped_result({'shares': [dict(path=path, subject_id='u1', subject_type='user')]}),
                    (-1, dict(visible=False, read=False, write=False)))

    def test_old_entry_does_not_activate_directory_grants(self):
        rule = dict(path='/team', subject_id='u1', subject_type='user', permission='rw', inherit=True)
        self.assertEqual(self.evaluate(dict(ce_permission=None, rules=[rule], target=dict(path='/team', kind='dir')))[1],
            dict(visible=False, read=False, write=False))


if __name__ == '__main__':
    unittest.main()
