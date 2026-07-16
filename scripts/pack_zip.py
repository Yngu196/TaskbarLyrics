# pack_zip.py — 打包 moeKoe-taskbar-lyrics 为 zip（正斜杠路径，兼容所有 zip 解析器）
# MoeKoeMusic 的 installPluginFromZip 要求 manifest.json 在 zip 根目录
# 例如: manifest.json, MoeKoeTaskbarLyrics.exe, ...（不能放在子目录下）
import zipfile, os, sys

src_dir = sys.argv[1]
zip_path = sys.argv[2]

with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(src_dir):
        # 排除 debug.log，避免调试日志进入分发包
        files = [f for f in files if f.lower() != "debug.log"]
        for file in files:
            file_path = os.path.join(root, file)
            rel_path = os.path.relpath(file_path, src_dir).replace('\\', '/')
            # 文件直接放在 zip 根目录
            zf.write(file_path, rel_path)

print(f"Packed {zip_path}")
