"""Keep SDK-cut documentation synchronized using AST literals, never SDK execution."""
import ast
from collections import Counter
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]


class SdkMemoryInventory(unittest.TestCase):
    def test_literal_catalogue_matches_documented_counts_and_matrix(self):
        tree = ast.parse((ROOT / 'dm/tools/sdk_patches.py').read_text())
        assignments = [node for node in tree.body if isinstance(node, ast.Assign)
                       and any(isinstance(target, ast.Name) and target.id == 'CUTS'
                               for target in node.targets)]
        self.assertEqual(len(assignments), 1)
        literal = assignments[0].value
        self.assertIsInstance(literal, ast.Dict)
        cuts = ast.literal_eval(literal)  # Parse static data; do not import/exec the module.
        self.assertEqual(len(cuts), len(literal.keys), 'duplicate SDK inventory keys')
        totals = Counter()
        per_build = {}
        for build, entries in cuts.items():
            self.assertRegex(build, r'^[0-9a-f]{32}$')
            counts = Counter()
            for entry in entries:
                self.assertEqual(len(entry), 4)
                label = entry[3]
                if label.startswith('AddressRegionManager constructor:'):
                    counts['N'] += 1
                else:
                    self.assertTrue(label.startswith('AllocateAddressRegion: alignment'), label)
                    counts['A'] += 1
            per_build[build] = counts
            totals.update(counts)
        document = (ROOT / 'docs/sdk-memory-compatibility.md').read_text()
        scope = re.search(r'(\d+) node-area cuts \+ (\d+) alignment cuts in (\d+) SDK builds \((\d+) entries\)', document)
        self.assertIsNotNone(scope)
        self.assertEqual(tuple(map(int, scope.groups())),
                         (totals['N'], totals['A'], len(cuts), sum(totals.values())))
        matrix = re.search(r'^\| SDK build key \|[^\n]*\n((?:\|[^\n]*\n)+)', document, re.M)
        self.assertIsNotNone(matrix)
        rows = re.findall(r'^\| `([0-9a-f]{32})` \| [^|]+ \| ([^|]+) \|', matrix.group(1), re.M)
        self.assertEqual(len(rows), len(cuts))
        self.assertEqual({build for build, _ in rows}, set(cuts))
        for build, notation in rows:
            expected = Counter()
            for count, kind in re.findall(r'(\d*)([NA])', notation):
                expected[kind] += int(count or 1)
            self.assertEqual(expected, per_build[build], build)


if __name__ == '__main__':
    unittest.main()
