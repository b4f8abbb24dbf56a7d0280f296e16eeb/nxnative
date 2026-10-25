"""Source-level startup placement checks; no original SDK is executed."""
from pathlib import Path
import unittest
HOST = Path(__file__).resolve().parents[1]

class SmokeWiring(unittest.TestCase):
    def test_preflight_before_guest_initialization_and_early_return(self):
        source = (HOST / "main.c").read_text()
        body = source.split("static void guest_main(void) {", 1)[1].split("struct dm_flags", 1)[0]
        main = source.split("int main(int argc, char **argv) {", 1)[1]
        self.assertLess(main.index("dm_csrng_smoke_prepare()"), main.index("front_run(guest_main)"))
        self.assertIn("g_csrng_smoke = dm_csrng_smoke_prepare();", main)
        self.assertLess(body.index('req("nninitInitializeSdkModule")'), body.index("dm_csrng_smoke_run()"))
        self.assertIn("if (g_csrng_smoke) { dm_csrng_smoke_run(); return; }", body)
        self.assertLess(body.index("dm_csrng_smoke_run()"), body.index('req("nninitStartup")'))
        self.assertLess(body.index("dm_csrng_smoke_run()"), body.index("run_module_inits()"))
        self.assertLess(body.index("dm_csrng_smoke_run()"), body.index('req("nnMain")'))

    def test_force_policy_not_disabled(self):
        source = (HOST / "hle_os.c").read_text()
        self.assertIn('HLE_FORCE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm"', source)
        self.assertNotIn("DM_CSRNG_SMOKE", (HOST / "loader.c").read_text())
        smoke = (HOST / "hle_csrng_smoke.c").read_text()
        self.assertIn("module_sym(sdk,", smoke)
        self.assertNotIn("guest_sym(", smoke)
        self.assertIn('"DM_SAVE"', smoke)

if __name__ == "__main__":
    unittest.main()
