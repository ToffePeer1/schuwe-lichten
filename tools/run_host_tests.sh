#!/usr/bin/env bash
# Runs every unit test folder in test/ on this PC without a system C++ compiler:
# zig (from PyPI, run through uv in a throwaway env) provides clang, and Unity
# is downloaded once into .pio/host_test. Usage: bash tools/run_host_tests.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/.pio/host_test"
UNITY_VERSION="2.6.0"
ZIGLANG_VERSION="0.16.0"
UNITY="$OUT/Unity-$UNITY_VERSION/src"

mkdir -p "$OUT"
if [ ! -f "$UNITY/unity.c" ]; then
  curl -sSL "https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v$UNITY_VERSION.tar.gz" \
    | tar -xz -C "$OUT"
fi

zig() { uv run --no-project --with "ziglang==$ZIGLANG_VERSION" python -m ziglang "$@"; }

zig cc -c "$UNITY/unity.c" -I"$UNITY" -o "$OUT/unity.o"
LIB_SOURCES=("$ROOT"/lib/laser_core/src/*.cpp)
failed=0
for dir in "$ROOT"/test/test_*/; do
  name="$(basename "$dir")"
  echo "=== $name"
  zig c++ -std=c++17 -Wall -Wextra -Werror -I"$UNITY" -I"$ROOT/lib/laser_core/src" \
    "${LIB_SOURCES[@]}" "$dir"test_main.cpp "$OUT/unity.o" -o "$OUT/$name.exe"
  if ! "$OUT/$name.exe" | tail -3; then
    failed=1
  fi
done
exit "$failed"
