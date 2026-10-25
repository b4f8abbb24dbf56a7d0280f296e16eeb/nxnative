"""All shipped profiles fit the native memory layout's actual host-page bounds."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'games'))
import game


class NativeAddressProfileTests(unittest.TestCase):
    def test_shipped_profile_boundaries(self):
        profiles = game.all_games()
        self.assertTrue(profiles)
        for profile in profiles:
            with self.subTest(profile=profile.id):
                reserve, alias, stack = (profile.int(key) for key in ('mem_reserve', 'mem_alias', 'mem_stack'))
                # No shipped profile uses a zero-size alias, including sys_resource=0 games.
                self.assertGreater(alias, 0)
                self.assertGreaterEqual(stack, 0)
                self.assertGreaterEqual(reserve, 0x10000000)
                self.assertLessEqual(alias + stack, reserve - 0x10000000)
                for page_size in (0x1000, 0x4000):
                    self.assertEqual((reserve | alias | stack | 0x10000000) % page_size, 0)


if __name__ == '__main__':
    unittest.main()
