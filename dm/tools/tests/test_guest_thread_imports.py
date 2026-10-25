"""Generated guest declarations need no game files and no Python clang package."""
import ctypes.util
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("thread_import_codegen", Path(__file__).resolve().parents[1] / "generate_guest_thread_imports.py")
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


class GuestThreadImports(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cxx = shutil.which("clang++")
        cls.library = None
        if not cls.cxx:
            cls.library = next((p for version in range(25, 13, -1) if (p := ctypes.util.find_library(f"clang-{version}"))), None)
        if not cls.cxx and not cls.library:
            raise unittest.SkipTest("AArch64-capable Clang driver or libclang frontend required for regeneration")

    def generate(self):
        return GEN.compile_imports(self.cxx) if self.cxx else GEN.frontend_imports(self.library)

    def test_checked_in_names_and_signature_digest(self):
        self.assertEqual(GEN.render(self.generate()), GEN.OUTPUT.read_text())

    def test_missing_and_duplicate_descriptors_are_rejected(self):
        imports = self.generate()
        missing = dict(imports); missing.pop(next(iter(missing)))
        with self.assertRaises(ValueError):
            GEN.render(missing)
        duplicate = dict.fromkeys(imports, next(iter(imports.values())))
        with self.assertRaises(ValueError):
            GEN.render(duplicate)

    def test_signature_change_changes_compiler_name_and_digest(self):
        original = self.generate()
        root = GEN.ROOT
        with tempfile.TemporaryDirectory() as tmp:
            try:
                GEN.ROOT = Path(tmp)
                for name in GEN.INPUTS:
                    target = GEN.ROOT / name; target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(root / name, target)
                schema = GEN.ROOT / GEN.INPUTS[0]
                schema.write_text(schema.read_text().replace("dm_guest_size", "unsigned long long"))
                changed = self.generate()
                self.assertNotEqual(original["CreateThread"], changed["CreateThread"])
                self.assertNotEqual(GEN.render(changed), GEN.OUTPUT.read_text())
            finally:
                GEN.ROOT = root

    def test_bool_parameter_mangling_and_return_type_digest(self):
        original = self.generate()
        root = GEN.ROOT
        with tempfile.TemporaryDirectory() as tmp:
            try:
                GEN.ROOT = Path(tmp)
                for name in GEN.INPUTS:
                    target = GEN.ROOT / name; target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(root / name, target)
                schema = GEN.ROOT / "dm/host/guest_mutex_signatures.inc"
                schema.write_text(schema.read_text().replace("dm_guest_bool", "int"))
                changed = self.generate()
                self.assertNotEqual(original["InitializeMutex"], changed["InitializeMutex"])
                # A return-only ABI change does not affect Itanium mangling;
                # the source digest must still invalidate the generated file.
                self.assertEqual(original["IsMutexLockedByCurrentThread"], changed["IsMutexLockedByCurrentThread"])
                self.assertNotEqual(GEN.render(changed), GEN.OUTPUT.read_text())
            finally:
                GEN.ROOT = root

    def test_check_fails_on_stale_header_without_rewriting(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in (*GEN.INPUTS, "dm/tools/generate_guest_thread_imports.py", "dm/host/guest_thread_imports.generated.inc"):
                target = root / name; target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(GEN.ROOT / name, target)
            generated = root / "dm/host/guest_thread_imports.generated.inc"
            generated.write_text(generated.read_text() + "// stale\n")
            before = generated.read_bytes()
            command = [sys.executable, str(root / "dm/tools/generate_guest_thread_imports.py"), "--check"]
            command += ["--cxx", self.cxx] if self.cxx else ["--libclang", self.library]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("stale", result.stderr)
            self.assertEqual(generated.read_bytes(), before)

    def test_object_parser_rejects_non_guest_formats(self):
        for data in [b"\xcf\xfa\xed\xfe" + bytes(60), b"\x7fELF\x01\x01" + bytes(58)]:
            with self.assertRaises(ValueError):
                GEN.elf_imports(data)
        # A host-GCC LP64 mangling proof must not count as AArch64 evidence.
        header = bytearray(64); header[:6] = b"\x7fELF\x02\x01"
        import struct
        struct.pack_into("<HHIQQQIHHHHHH", header, 16, 1, 62, 1, 0, 0, 0, 0, 64, 0, 0, 64, 0, 0)
        with self.assertRaisesRegex(ValueError, "AArch64"):
            GEN.elf_imports(header)


if __name__ == "__main__":
    unittest.main()
