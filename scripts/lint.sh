#!/bin/sh
# The checks CI runs, locally: formatting (clang-format), static analysis (clang-tidy, on
# a configured build's compile_commands.json) and shell scripts (shellcheck).
#
#   scripts/lint.sh          # check (build dir: build, or $BUILD_DIR)
#   scripts/lint.sh --fix    # only reformat, in place
set -eu
cd "$(dirname "$0")/.."

# Homebrew's llvm is keg-only: its tools aren't on PATH by default.
for dir in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do
    [ -d "$dir" ] && PATH="$dir:$PATH"
done

sources() { git ls-files -z 'src/*.cpp' 'src/*.h' ':!src/ui/lv_conf.h'; }

if [ "${1:-}" = "--fix" ]; then
    sources | xargs -0 clang-format -i
    exit
fi
sources | xargs -0 clang-format --dry-run --Werror
git ls-files -z '*.sh' | xargs -0 shellcheck

build=${BUILD_DIR:-build}
if [ ! -f "$build/compile_commands.json" ]; then
    echo "clang-tidy needs a configured build: cmake -B $build" >&2
    exit 1
fi
git ls-files -z 'src/*.cpp' | xargs -0 -n 1 -P "$(getconf _NPROCESSORS_ONLN)" clang-tidy -p "$build" --quiet
