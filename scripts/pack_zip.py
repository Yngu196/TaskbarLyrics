# pack_zip.py — 打包 moeKoe-taskbar-lyrics 为 zip（正斜杠路径，兼容所有 zip 解析器）
import zipfile, os, sys

src_dir = sys.argv[1]
zip_path = sys.argv[2]

with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(src_dir):
        for file in files:
            file_path = os.path.join(root, file)
            arcname = os.path.relpath(file_path, src_dir).replace('\\', '/')
            zf.write(file_path, arcname)

print(f"Packed {zip_path}")
