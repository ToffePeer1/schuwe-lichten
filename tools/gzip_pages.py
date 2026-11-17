"""PlatformIO pre-build script: gzip the web pages before they are embedded in the firmware.

The pages (both languages) and the pattern list are about 270 KB as plain text but well under
100 KB gzipped, and every browser unpacks gzip by itself. This script writes
build/web/<name>.gz for each file in src/web (deterministic: no timestamp, so an unchanged page
gives an identical file and no rebuild), checks that each one unpacks to the original, and the
firmware serves them with "Content-Encoding: gzip" (src/web_ui.cpp). build/ is gitignored.
The mock firmware keeps serving the plain files from src/web.
"""

import gzip
from pathlib import Path

Import("env")  # noqa: F821 (provided by PlatformIO/SCons)

PAGES = ("index.html", "admin.html", "patterns.json")

root = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
source_dir = root / "src" / "web"
target_dir = root / "build" / "web"
target_dir.mkdir(parents=True, exist_ok=True)

total_plain = total_packed = 0
for name in PAGES:
    plain = (source_dir / name).read_bytes()
    packed = gzip.compress(plain, compresslevel=9, mtime=0)
    if gzip.decompress(packed) != plain:
        raise RuntimeError(f"gzip_pages.py: {name} does not unpack to the original")
    target = target_dir / f"{name}.gz"
    if not target.exists() or target.read_bytes() != packed:
        target.write_bytes(packed)
    total_plain += len(plain)
    total_packed += len(packed)

print(f"gzip_pages.py: web pages {total_plain // 1024} KB -> {total_packed // 1024} KB gzipped")
