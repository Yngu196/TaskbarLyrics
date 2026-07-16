# pack_zip.py — 打包 moeKoe-taskbar-lyrics 为 zip（正斜杠路径，兼容所有 zip 解析器）
# MoeKoeMusic 的 installPluginFromZip 通过 entryName.split('/') 检测:
#   parts.length === 2 && parts[1] === 'manifest.json'
# 因此所有文件必须放在一个顶层目录内，例如:
#   moeKoe-taskbar-lyrics/manifest.json
# 根目录散放 (parts.length === 1) 会报"未找到 manifest.json"
import zipfile, os, sys

src_dir = sys.argv[1]
zip_path = sys.argv[2]

# 使用源目录名作为 zip 内的根目录名
root_dir = os.path.basename(os.path.normpath(src_dir))

with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(src_dir):
        # 排除 debug.log，避免调试日志进入分发包
        files = [f for f in files if f.lower() != "debug.log"]
        for file in files:
            file_path = os.path.join(root, file)
            rel_path = os.path.relpath(file_path, src_dir).replace('\\', '/')
            # 所有文件放在 root_dir/ 下，确保 depth=2 通过 MoeKoeMusic 检测
            arcname = f"{root_dir}/{rel_path}"
            zf.write(file_path, arcname)

print(f"Packed {zip_path}")
