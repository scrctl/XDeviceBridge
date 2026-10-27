"""Keep public RPC names and C symbols from silently disappearing from reference docs."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ReferenceCoverage(unittest.TestCase):
    def test_all_dispatched_rpc_names_documented(self):
        sources = '\n'.join((ROOT / name).read_text(encoding="utf-8") for name in
                            ('src/server/Backend.cpp', 'src/server/Server.cpp'))
        methods = set(re.findall(r'(?:method|request\["method"\])\s*==\s*"([a-z_.]+)"', sources))
        methods.add('hello')
        for language in ('', '_zh'):
            with self.subTest(language=language or 'en'):
                doc = (ROOT / f'docs/API{language}.md').read_text(encoding="utf-8")
                headings = set()
                for line in doc.splitlines():
                    if line.startswith('### '):
                        headings.update(re.findall(r'`([a-z_.]+)`', line))
                self.assertEqual(methods, headings)
                self.assertNotIn('pairing.import', headings)

    def test_exported_c_symbols_documented(self):
        header = (ROOT / 'include/xdb/xdb.h').read_text(encoding="utf-8")
        symbols = set(re.findall(r'\b(xdb_[a-z_]+)\s*\(', header))
        doc = (ROOT / 'docs/C_API.md').read_text(encoding="utf-8")
        headings = set(re.findall(r'^### `(xdb_[a-z_]+)`', doc, re.M))
        self.assertEqual(symbols, headings)

if __name__ == '__main__':
    unittest.main()
