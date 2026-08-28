#!/usr/bin/env python3
# verify_package.py — 校验双架构发布包中各 PE 文件架构是否与目录约定一致
#
# 用法:
#   python verify_package.py <moeKoe-taskbar-lyrics 目录>
#
# 校验规则:
#   根目录 MoeKoeTaskbarLyrics.exe (Launcher)  = AMD64
#   x64/   下所有 .exe / .dll                 = AMD64
#   arm64/ 下所有 .exe / .dll                 = ARM64
#
# 实现：纯 Python 标准库解析 PE 文件的 IMAGE_FILE_MACHINE 字段
#   DOS header e_lfanew(0x3C, DWORD) -> PE signature "PE\0\0" -> Machine(HWORD)
# 逐文件输出 [OK]/[FAIL]，最终给出 PASS/FAIL 汇总；exit code 0/1
import os
import struct
import sys

IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_ARM64 = 0xAA64

MACHINE_NAMES = {
    IMAGE_FILE_MACHINE_I386: "I386",
    IMAGE_FILE_MACHINE_AMD64: "AMD64",
    IMAGE_FILE_MACHINE_ARM64: "ARM64",
}


def read_pe_machine(path):
    """返回 (machine:int|None, err:str|None)。"""
    try:
        with open(path, "rb") as f:
            dos = f.read(0x40)
            if len(dos) < 0x40 or dos[:2] != b"MZ":
                return None, "not a PE file (no MZ header)"
            (e_lfanew,) = struct.unpack("<I", dos[0x3C:0x40])
            f.seek(e_lfanew)
            if f.read(4) != b"PE\0\0":
                return None, "invalid PE signature"
            (machine,) = struct.unpack("<H", f.read(2))
            return machine, None
    except (OSError, struct.error) as exc:
        return None, "read error: {0}".format(exc)


def expected_arch(rel, name):
    """返回该文件期望的 Machine 值；不在校验范围内返回 None。"""
    lower = name.lower()
    if not lower.endswith((".exe", ".dll")):
        return None
    parts = rel.split(os.sep)
    if len(parts) == 1:
        # 根目录：仅校验 Launcher（MoeKoeTaskbarLyrics.exe）
        return IMAGE_FILE_MACHINE_AMD64 if lower == "moekoetaskbarlyrics.exe" else None
    top = parts[0].lower()
    if top == "x64":
        return IMAGE_FILE_MACHINE_AMD64
    if top == "arm64":
        return IMAGE_FILE_MACHINE_ARM64
    return None


def main():
    if len(sys.argv) != 2:
        print("Usage: python verify_package.py <moeKoe-taskbar-lyrics dir>")
        return 2
    root = os.path.abspath(sys.argv[1])
    if not os.path.isdir(root):
        print("[ERROR] Directory not found: {0}".format(root))
        return 2

    checked = 0
    failed = 0
    for dirpath, dirs, files in os.walk(root):
        for name in sorted(files):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root)
            expected = expected_arch(rel, name)
            if expected is None:
                continue
            checked += 1
            machine, err = read_pe_machine(full)
            if err is not None:
                print("[FAIL] {0:<52} {1}".format(rel, err))
                failed += 1
                continue
            actual = MACHINE_NAMES.get(machine, "0x{0:04X}".format(machine))
            expected_name = MACHINE_NAMES.get(expected, "0x{0:04X}".format(expected))
            if machine == expected:
                print("[OK]   {0:<52} {1}".format(rel, actual))
            else:
                print("[FAIL] {0:<52} expected {1}, got {2}".format(rel, expected_name, actual))
                failed += 1

    print()
    if failed:
        print("[FAIL] Package architecture check ({0}/{1} passed)".format(checked - failed, checked))
        return 1
    print("[PASS] Package architecture check ({0} file(s) verified)".format(checked))
    return 0


if __name__ == "__main__":
    sys.exit(main())
