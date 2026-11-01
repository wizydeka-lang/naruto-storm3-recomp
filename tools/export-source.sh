#!/usr/bin/env bash
# Copy the shareable project (no game files, generated code or builds) into a
# new directory, ready to become a fresh public repository.
# Usage: bash tools/export-source.sh <empty target directory>
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
target="${1:?usage: export-source.sh <empty target directory>}"
if [[ -e "$target" && -n "$(ls -A -- "$target")" ]]; then
  printf '%s is not empty.\n' "$target" >&2
  exit 1
fi

# Only these paths are shared; everything else (assets/, generated/default/,
# out/, logs, captures) stays private because it is derived from the game.
allow=(
  CMakeLists.txt CMakePresets.json naruto_manifest.toml README.md
  generated/rexglue.cmake src/ config/ patches/ tools/ tests/ docs/
)

cd -- "$project_root"
mkdir -p -- "$target"
git ls-files --cached --others --exclude-standard -- "${allow[@]}" |
  while IFS= read -r file; do
    [[ -f "$file" && "$file" != *__pycache__* ]] || continue
    mkdir -p -- "$target/$(dirname -- "$file")"
    cp -- "$file" "$target/$file"
  done

cat > "$target/.gitignore" << 'EOF'
# Game files are supplied by each user and never committed.
/assets/
# Translated game code, regenerated from assets/default.xex during the build.
/generated/default/
# Build outputs contain translated game code too.
/out/
*.rdc
__pycache__/
EOF
printf 'Exported %s files to %s\n' "$(find "$target" -type f | wc -l)" "$target"
