"""Portable admission execution and narrow production wiring checks; not Metal execution."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]
SOURCE = (ROOT / "dm/gfx/nvn/nvn_mtl.m").read_text()
class CubeCopy(unittest.TestCase):
    def test_policy(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d) / "policy"
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                ["-std=c11", "-Wall", "-Wextra", "-Werror"] +
                shlex.split(os.environ.get("NVN_CUBE_COPY_CFLAGS", "")) +
                [str(ROOT / "dm/gfx/tests/t_cube_copy_policy.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    def test_opt_in_and_no_gpu_work_during_binding(self):
        self.assertIn('v = e && !strcmp(e, "1")', SOURCE)
        fit = SOURCE.split("static id<MTLTexture> tex_fit(", 1)[1].split("// tex_fit for", 1)[0]
        self.assertIn("cube_copy_bound(t, want, rid)", fit)
        self.assertNotIn("cube_copy_prepare_texture", fit)
        self.assertNotIn("blit()", fit)
    def test_no_previous_consumer_content_reuse(self):
        self.assertIn("c->prepared != g_cube_copy_use", SOURCE)
        self.assertIn("cube_copy_begin(); cube_copy_prepare_shader(cs, 5)", SOURCE)
        self.assertIn("cube_copy_begin(); cube_copy_prepare_shader(vs, 0)", SOURCE)
        self.assertIn("cube_copy_prepare_shader(pr->sh[0], 0); cube_copy_prepare_shader(pr->sh[1], 1)", SOURCE)
        self.assertIn("!g_defer_nact && !g_defer_replaying && !g_fz.active", SOURCE)
    def test_helper_scope_fastchain_and_pending_clear_guards(self):
        self.assertIn("!cube_copy_on() || !g_cube_copy_binding", SOURCE)
        self.assertIn("mdx_on && !g_cube_copy_last_bound && mdx_next", SOURCE)
        self.assertIn("g_rp_layered = 0; lc_land(); g_rp_layered = layered", SOURCE)
        self.assertIn("rp_flush();   // lazy and load-action clears", SOURCE)
        self.assertIn("mtl_pf_dpf(t.pixelFormat) != NVN_DPF_FLOAT", SOURCE)
        self.assertIn("g_cube_copy_binding = 1; encode_args(ce, cs, 5, 0); g_cube_copy_binding = 0", SOURCE)
        geometry = (ROOT / "dm/gfx/nvn/nvn_geometry.inc").read_text()
        self.assertNotIn("g_cube_copy_binding = 1", geometry)
    def test_resource_and_view_contracts(self):
        copy = SOURCE.split("static int cube_copy_prepare_texture(", 1)[1].split("static id<MTLTexture> dummy_tex", 1)[0]
        self.assertIn("up_flush();", copy)
        self.assertIn("defer_obj_unref(t)", copy)
        self.assertIn("res_add(storage)", copy)
        self.assertIn("nvn_cube_copy_evictable", copy)
        self.assertIn("swizzle:t.swizzle", copy)
        self.assertIn("enc_b_copy_texture(b, t, 0, l", copy)
        self.assertNotIn("q_cb()", copy)
if __name__ == "__main__": unittest.main()
