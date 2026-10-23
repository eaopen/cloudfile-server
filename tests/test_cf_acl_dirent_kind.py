"""Cheap contract: native batch sends file vs directory kind to Hub Authority."""
from pathlib import Path
import unittest


class NativeDirentKindTests(unittest.TestCase):
    def test_native_permission_batch_uses_real_dirent_mode(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'common' / 'cf-acl.c').read_text(encoding='utf-8')
        chunk = source.split('filter_chunk (', 1)[1].split('\nGList *\ncf_acl_filter_dirents', 1)[0]
        self.assertIn('"mode",&mode', chunk)
        self.assertIn('S_ISDIR(mode) ? "dir" : "file"', chunk)
        self.assertIn('"path",path,"kind",kind,"action","read"', chunk)
        self.assertIn('cf_v2_acl_check_json(seaf,payload,body)', chunk)
        self.assertIn('json_array_size(permissions)!=size', chunk)


if __name__ == '__main__':
    unittest.main()
