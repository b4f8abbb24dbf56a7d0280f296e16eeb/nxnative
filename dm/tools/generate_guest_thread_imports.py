#!/usr/bin/env python3
"""Generate checked-in guest imports; not a player/build-time dependency.

The normal path compiles AArch64 ELF relocation anchors with Clang. --libclang
allows its target-aware frontend to generate the same names on maintainer hosts
with only libclang installed; CI must still check the ELF object path. Neither
path validates Nintendo SDK layout/TLS or executes a guest function.
"""
import argparse
import ctypes as ct
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INPUTS = ("dm/host/guest_thread_signatures.inc", "dm/host/guest_thread_abi.h", "dm/tools/guest_thread_imports.cpp", "dm/host/guest_mutex_signatures.inc")
TARGET = "aarch64-none-elf"
OUTPUT = ROOT / "dm/host/guest_thread_imports.generated.inc"


def expected_names():
    return re.findall(r"^DM_GUEST_(?:THREAD|MUTEX)_SIGNATURE\((\w+),",
                      (ROOT / INPUTS[0]).read_text() + (ROOT / INPUTS[3]).read_text(), re.M)


def elf_imports(data):
    if data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("generation requires little-endian ELF64")
    hdr = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    kind, machine, _, _, _, shoff, _, _, _, _, shsize, shnum, shstr = hdr
    if kind != 1 or machine != 183 or shsize != 64:
        raise ValueError("generation requires an AArch64 relocatable object")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shsize) for i in range(shnum)]
    def blob(section):
        return data[section[4]:section[4] + section[5]]
    def string(table, offset):
        return table[offset:table.index(0, offset)].decode("ascii")
    names = blob(sections[shstr])
    result = {}
    for section in sections:
        name = string(names, section[0])
        if not name.startswith(".rela.dm_import."):
            continue
        key = name.removeprefix(".rela.dm_import.")
        if key in result or section[1] != 4 or section[5] != 24 or section[9] != 24:
            raise ValueError(f"expected exactly one RELA anchor for {key}")
        offset, info, addend = struct.unpack("<QQq", blob(section))
        if offset or addend or info & 0xffffffff != 257:
            raise ValueError(f"expected an unadjusted R_AARCH64_ABS64 import for {key}")
        symtab = sections[section[6]]
        symbol = struct.unpack_from("<IBBHQQ", data, symtab[4] + (info >> 32) * symtab[9])
        if symbol[3]:
            raise ValueError(f"anchor {key} must reference an undefined guest function")
        result[key] = string(blob(sections[symtab[6]]), symbol[0])
    return result


def compile_imports(cxx):
    with tempfile.TemporaryDirectory(prefix="dm-thread-imports-") as tmp:
        obj = Path(tmp) / "imports.o"
        subprocess.run([cxx, "--target=" + TARGET, "-std=c++17", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
                        "-Wall", "-Wextra", "-Werror", "-c", str(ROOT / INPUTS[2]), "-o", str(obj)], check=True)
        return elf_imports(obj.read_bytes())


def frontend_imports(library):
    # No Python clang package needed. Public libclang C API only; the generator
    # does not parse/construct C++ manglings itself.
    lib = ct.CDLL(library)
    class String(ct.Structure):
        _fields_ = [("data", ct.c_void_p), ("flags", ct.c_uint)]
    class Cursor(ct.Structure):
        _fields_ = [("kind", ct.c_uint), ("xdata", ct.c_int), ("data", ct.c_void_p * 3)]
    def api(name, result, *args):
        fn = getattr(lib, name); fn.restype = result; fn.argtypes = args
        return fn
    create = api("clang_createIndex", ct.c_void_p, ct.c_int, ct.c_int)
    dispose_index = api("clang_disposeIndex", None, ct.c_void_p)
    parse = api("clang_parseTranslationUnit", ct.c_void_p, ct.c_void_p, ct.c_char_p, ct.POINTER(ct.c_char_p), ct.c_int, ct.c_void_p, ct.c_uint, ct.c_uint)
    dispose_tu = api("clang_disposeTranslationUnit", None, ct.c_void_p)
    get_string = api("clang_getCString", ct.c_char_p, String)
    dispose_string = api("clang_disposeString", None, String)
    def text(value):
        value_text = get_string(value).decode(); dispose_string(value); return value_text
    index = create(0, 0)
    args = [b"--target=" + TARGET.encode(), b"-std=c++17", b"-ffreestanding", b"-Wall", b"-Wextra", b"-Werror"]
    tu = parse(index, str(ROOT / INPUTS[2]).encode(), (ct.c_char_p * len(args))(*args), len(args), None, 0, 0)
    if not tu:
        dispose_index(index); raise ValueError("libclang could not parse the guest declarations")
    try:
        ndiags = api("clang_getNumDiagnostics", ct.c_uint, ct.c_void_p)(tu)
        diagnostic = api("clang_getDiagnostic", ct.c_void_p, ct.c_void_p, ct.c_uint)
        severity = api("clang_getDiagnosticSeverity", ct.c_uint, ct.c_void_p)
        spelling = api("clang_getDiagnosticSpelling", String, ct.c_void_p)
        dispose_diag = api("clang_disposeDiagnostic", None, ct.c_void_p)
        for i in range(ndiags):
            d = diagnostic(tu, i); level = severity(d); msg = text(spelling(d)); dispose_diag(d)
            if level >= 2:
                raise ValueError(msg)
        target = api("clang_getTranslationUnitTargetInfo", ct.c_void_p, ct.c_void_p)(tu)
        triple = text(api("clang_TargetInfo_getTriple", String, ct.c_void_p)(target))
        width = api("clang_TargetInfo_getPointerWidth", ct.c_int, ct.c_void_p)(target)
        api("clang_TargetInfo_dispose", None, ct.c_void_p)(target)
        if not triple.startswith("aarch64-") or not triple.endswith("-elf") or width != 64:
            raise ValueError(f"unexpected frontend target: {triple}, {width}")
        get_name = api("clang_getCursorSpelling", String, Cursor)
        get_mangling = api("clang_Cursor_getMangling", String, Cursor)
        visitor_type = ct.CFUNCTYPE(ct.c_uint, Cursor, Cursor, ct.c_void_p)
        result = {}
        @visitor_type
        def visit(cursor, parent, data):
            if cursor.kind == 8:  # CXCursor_FunctionDecl
                name = text(get_name(cursor))
                if name in expected_names():
                    result[name] = text(get_mangling(cursor))
            return 2  # CXChildVisit_Recurse
        cursor = api("clang_getTranslationUnitCursor", Cursor, ct.c_void_p)(tu)
        api("clang_visitChildren", ct.c_uint, Cursor, visitor_type, ct.c_void_p)(cursor, visit, None)
        return result
    finally:
        dispose_tu(tu); dispose_index(index)


def render(imports):
    expected = expected_names()
    if set(imports) != set(expected) or len(set(imports.values())) != len(expected):
        raise ValueError(f"incomplete or duplicate guest imports: {imports}")
    digest = hashlib.sha256()
    for path in INPUTS:
        digest.update(path.encode() + b"\0" + (ROOT / path).read_bytes() + b"\0")
    out = "// Generated by dm/tools/generate_guest_thread_imports.py; do not edit.\n"
    out += f"// Guest target: {TARGET}; declarations SHA-256: {digest.hexdigest()}\n"
    for name in expected:
        symbol = imports[name]
        if not re.fullmatch(r"_Z[A-Za-z0-9_]+", symbol):
            raise ValueError(f"not a compiler-generated Itanium import: {symbol!r}")
        out += f'DM_GUEST_THREAD_IMPORT({name}, "{symbol}")\n'
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++", help="Clang driver (CI/default: validates AArch64 ELF relocations)")
    parser.add_argument("--libclang", help="maintainer fallback: target-aware libclang frontend shared library")
    parser.add_argument("--check", action="store_true", help="fail rather than overwrite a stale generated file")
    args = parser.parse_args()
    imports = frontend_imports(args.libclang) if args.libclang else compile_imports(args.cxx)
    generated = render(imports)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != generated:
            parser.exit(1, "guest thread imports are stale; regenerate and check in the result\n")
    else:
        OUTPUT.write_text(generated)
    print("PASS: guest thread imports " + ("match" if args.check else "generated") +
          (" (AArch64 ELF-target frontend; object check still required in CI)" if args.libclang else " (AArch64 ELF relocation check)"))


if __name__ == "__main__":
    main()
