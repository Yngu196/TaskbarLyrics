# pack_zip.py — 打包 moeKoe-taskbar-lyrics 为 zip（正斜杠路径，兼容所有 zip 解析器）
# MoeKoeMusic 的 installPluginFromZip 通过 entryName.split('/') 检测:
#   parts.length === 2 && parts[1] === 'manifest.json'
# 因此所有文件必须放在一个顶层目录内，例如:
#   moeKoe-taskbar-lyrics/manifest.json
# 根目录散放 (parts.length === 1) 会报"未找到 manifest.json"
import json
import os
import sys
import zipfile


def fail(message):
    print("[ERROR] " + message)
    sys.exit(1)


if len(sys.argv) != 3:
    fail("Usage: python pack_zip.py <moeKoe-taskbar-lyrics dir> <output.zip>")

src_dir = os.path.abspath(sys.argv[1])
zip_path = os.path.abspath(sys.argv[2])

if not os.path.isdir(src_dir):
    fail("Source directory not found: " + src_dir)
if not os.path.isdir(os.path.join(src_dir, "x64")) or not os.path.isdir(os.path.join(src_dir, "arm64")):
    fail("Both x64/ and arm64/ directories are required; run 'build.cmd release all' first")
if os.path.commonpath([zip_path, src_dir]) == src_dir:
    fail("Output zip must be outside the source directory")

manifest_path = os.path.join(src_dir, "manifest.json")
if not os.path.isfile(manifest_path):
    fail("manifest.json missing at package root")
try:
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    if not manifest.get("version"):
        fail("manifest.json has no version field")
except (OSError, ValueError) as exc:
    fail("Cannot parse manifest.json: {0}".format(exc))

# 使用源目录名作为 zip 内的根目录名
root_dir = os.path.basename(os.path.normpath(src_dir))

license_path = os.path.join(src_dir, "LICENSE")
if not os.path.isfile(license_path):
    license_path = os.path.join(os.path.dirname(src_dir), "LICENSE")
if not os.path.isfile(license_path):
    license_path = os.path.join(os.getcwd(), "LICENSE")
if not os.path.isfile(license_path):
    fail("LICENSE not found in package, parent directory, or repository root")

with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(src_dir):
        # 排除 debug.log 与调试符号（.pdb），避免它们进入分发包
        files = [f for f in files if f.lower() != "debug.log" and not f.lower().endswith(".pdb")]
        for file in files:
            file_path = os.path.join(root, file)
            rel_path = os.path.relpath(file_path, src_dir).replace("\\", "/")
            # 所有文件放在 root_dir/ 下，确保 depth=2 通过 MoeKoeMusic 检测
            zf.write(file_path, "{0}/{1}".format(root_dir, rel_path))
    # 许可证必须随包分发（Apache-2.0 要求），从仓库根补入
    zf.write(license_path, root_dir + "/LICENSE")

print("Packed {0} (v{1})".format(zip_path, manifest["version"]))
