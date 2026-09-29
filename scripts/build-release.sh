#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build/release"
install_dir="${1:-$repo_root/dist}"

cmake -S "$repo_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
(cd "$repo_root/web" && npm ci && npm run typecheck && npm test && npm run build)
cmake --build "$build_dir" -j2
ctest --test-dir "$build_dir" --output-on-failure
cmake --install "$build_dir" --prefix "$install_dir"
printf 'Aegis installed to %s\n' "$install_dir/bin"
