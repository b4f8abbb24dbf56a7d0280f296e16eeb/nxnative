"""Full NSO identity metadata with synthetic headers/fixups only.

Compile the actual read_layout source body against the real module_t, without
Apple-only mapping code; execute only this host parser, never guest code. Run
only the rewriter's identity assignments and fixups writer, not the rewriter.
"""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
HOST = REPO / 'dm/host'
BID = bytes(range(32)).hex()
HEADER = ''.join(f'# {key}\t{value}\n' for key, value in (
    ('PATCH_OFF', '0x10000'), ('KEY_PAGE_OFF', '0x8000'),
    ('KEY_PAGE_SIZE', '0x4000'), ('TEXT_END', '0x1000'),
    ('RO_OFF', '0x4000'), ('DATA_OFF', '0x8000'),
    ('KEY_K18_IDX', '0'), ('KEY_KTLS_IDX', '1'),
    ('KEY_KTLR_IDX', '2'), ('KEY_KGATE_IDX', '3')))


class FullBuildIdentity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the real read_layout parser')
        cls.temp = tempfile.TemporaryDirectory(prefix='dm-full-build-id-')
        cls.root = Path(cls.temp.name)
        loader = (HOST / 'loader.c').read_text()
        start = loader.index('static void read_layout(module_t *m, const char *dir) {')
        end = loader.index('\n// Frame step state', start)
        source = cls.root / 'layout.c'
        source.write_text('''#include "dm.h"
#undef die
#define die(...) do { fprintf(stderr, __VA_ARGS__); fputc('\\n', stderr); exit(19); } while (0)
enum { KEY_N = 4 };
''' + loader[start:end] + '''
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    module_t m = {0};
    strcpy(m.name, "main");
    /* Missing metadata must clear any prior identity, not retain stale data. */
    memset(m.build_id, 'a', sizeof m.build_id);
    memset(m.full_build_id, 'b', sizeof m.full_build_id);
    read_layout(&m, argv[1]);
    printf("%s\\n%s\\n", m.build_id, m.full_build_id);
    return 0;
}
''')
        cls.binary = cls.root / 'layout'
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(HOST), '-I' + str(HOST / 'tests/memory_profile'),
                        str(source), '-o', str(cls.binary)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'temp'):
            cls.temp.cleanup()

    def parse(self, rows, status=0):
        (self.root / 'main.fixups.tsv').write_text(HEADER + rows)
        result = subprocess.run([str(self.binary), str(self.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode, status, result.stderr)
        return result.stdout.splitlines()

    def test_legacy_short_id_and_missing_identity(self):
        self.assertEqual(self.parse(f'# BUILD_ID\t{BID[:32]}\n'), [BID[:32], ''])
        self.assertEqual(self.parse(''), ['', ''])

    def test_legacy_short_id_retains_malformed_rejection(self):
        for row in ('# BUILD_ID\n', '# BUILD_ID\tnothex\n'):
            with self.subTest(row=row):
                self.parse(row, status=19)

    def test_exact_full_identity_in_either_order(self):
        rows = [f'# BUILD_ID\t{BID[:32]}\n', f'# BUILD_ID_FULL\t{BID}\n']
        self.assertEqual(self.parse(''.join(rows)), [BID[:32], BID])
        self.assertEqual(self.parse(''.join(reversed(rows))), [BID[:32], BID])

    def test_full_identity_rejects_malformed_or_truncated_values(self):
        for value in ('', BID[:32], BID[:-1], BID + '0', BID.upper(), BID[:-1] + 'g', BID + ' extra'):
            with self.subTest(value=value):
                self.parse(f'# BUILD_ID\t{BID[:32]}\n# BUILD_ID_FULL\t{value}\n', status=19)

    def test_duplicate_full_identity_is_not_accepted(self):
        for value in (BID, 'f' * 64):
            with self.subTest(value=value):
                self.parse(f'# BUILD_ID\t{BID[:32]}\n# BUILD_ID_FULL\t{BID}\n# BUILD_ID_FULL\t{value}\n', status=19)

    def test_full_identity_requires_matching_short_identity(self):
        self.parse(f'# BUILD_ID_FULL\t{BID}\n', status=19)
        self.parse(f'# BUILD_ID\t{"f" * 32}\n# BUILD_ID_FULL\t{BID}\n', status=19)

    def test_rewriter_emits_full_and_compatible_short_identity(self):
        tree = ast.parse((REPO / 'dm/tools/patch_nso.py').read_text())
        assignments = [node for node in tree.body if isinstance(node, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id in ('BUILD_ID', 'BUILD_ID_FULL')
                               for t in node.targets)]
        self.assertEqual(len(assignments), 2)
        writers = [node for node in tree.body if isinstance(node, ast.With)
                   and any(isinstance(item.context_expr, ast.Call)
                           and any(isinstance(value, ast.Constant) and value.value == 'fixups.tsv'
                                   for value in ast.walk(item.context_expr)) for item in node.items)]
        self.assertEqual(len(writers), 1)
        scope = {'d': bytes(0x40) + bytes(range(32)),
                 'P': lambda name: str(self.root / ('writer.' + name)),
                 'PATCH_OFF': 0x10000, 'KEY_PAGE_OFF': 0x8000, 'TEXT_END': 0x1000,
                 'RO_OFF': 0x4000, 'DATA_OFF': 0x8000, 'FAR': False,
                 'KEY_INDEX': {'K18': 0, 'KTLS': 1, 'KTLR': 2, 'KGATE': 3},
                 'FS_STATE_OFF': 0, 'RENDER_SCALE': 0}
        exec(compile(ast.Module(body=assignments + writers, type_ignores=[]), '<metadata writer>', 'exec'), scope)
        output = (self.root / 'writer.fixups.tsv').read_text()
        self.assertIn(f'# BUILD_ID\t{BID[:32]}\n', output)
        self.assertIn(f'# BUILD_ID_FULL\t{BID}\n', output)
        (self.root / 'main.fixups.tsv').write_text(output)
        result = subprocess.run([str(self.binary), str(self.root)], check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.splitlines(), [BID[:32], BID])


if __name__ == '__main__':
    unittest.main()
