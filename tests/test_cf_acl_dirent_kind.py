"""Cheap contract: native batch sends file vs directory kind to Hub Authority.

An entry with no Authority kind (a native symlink) must be skipped on its own:
aborting the chunk hides every sibling on the page.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def chunk():
    source = (ROOT / 'common' / 'cf-acl.c').read_text(encoding='utf-8')
    return source.split('filter_chunk (', 1)[1].split('\nGList *\ncf_acl_filter_dirents', 1)[0]


class NativeDirentKindTests(unittest.TestCase):
    def test_native_permission_batch_uses_real_dirent_mode(self):
        text = chunk()
        self.assertIn('"mode",&mode', text)
        self.assertIn('S_ISDIR(mode) ? "dir" : "file"', text)
        self.assertIn('"path",path,"kind",kind,"action","read"', text)
        self.assertIn('cf_v2_acl_check_json(seaf,payload,body)', text)
        self.assertIn('json_array_size(permissions)!=size', text)

    def test_invalid_name_still_aborts_the_chunk(self):
        text = chunk()
        name_guard = text.split('g_object_get(item,"obj_name",&name,"mode",&mode,NULL);', 1)[1]
        name_guard = name_guard.split('/* A dirent that is neither', 1)[0]
        self.assertIn('strchr(name,\'/\')', name_guard)
        self.assertIn('goto done;', name_guard)

    def test_unknown_dirent_kind_skips_only_that_child(self):
        text = chunk()
        mode_guard = text.split('if (!S_ISDIR(mode) && !S_ISREG(mode)) {', 1)[1].split('}', 1)[0]
        self.assertIn('continue;', mode_guard)
        self.assertNotIn('goto done', mode_guard)

    def test_permissions_map_to_the_checked_children_only(self):
        text = chunk()
        mapping = text.split('json_array_size(permissions)!=size', 1)[1].split('result=0;', 1)[0]
        self.assertIn('included=g_list_append(included,item);', text)
        self.assertIn('GList *p=included;', mapping)
        self.assertEqual(mapping.count('p=included;'), 2)
        self.assertNotIn('GList *p=begin;', mapping)
        self.assertIn('g_list_free(included);', text)

    def test_all_children_skipped_is_not_a_batch_failure(self):
        text = chunk()
        self.assertIn('if (!size) { result=0; goto done; }', text)


if __name__ == '__main__':
    unittest.main()
