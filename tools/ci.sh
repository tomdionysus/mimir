#!/bin/sh
#
# Mimir - The Distributed Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One CI step on Debian 13, as the nodes have it, in GitHub's runner, a local
# container or a build server; .github/workflows/ci.yml runs each as its own job.
#
#   tools/ci.sh check      the header block on every source, and clang-format
#   tools/ci.sh release    g++ release build, warnings as errors, and the tests
#   tools/ci.sh clang      the same with clang++ (and -Wthread-safety)
#   tools/ci.sh asan       AddressSanitizer and UndefinedBehaviorSanitizer
#   tools/ci.sh tsan       ThreadSanitizer
#   tools/ci.sh coverage   a coverage build's tests; lcov report build-ci/coverage.info
#   tools/ci.sh client     hiredis against a real three-node cluster (ruling 44)
#   tools/ci.sh tidy       clang-tidy's bugprone-use-after-move over every source
#                          and Mimir's headers, warnings as errors
#
# Builds go under build-ci/ (MIMIR_CI_OUT to move it), one job per CPU
# (MIMIR_CI_JOBS to set it: 2 on the shared build servers). Dannan is fetched at the
# pinned tag from the sibling checkout when there is one, else from its
# repository (MIMIR_DANNAN_REPOSITORY to name another). Needs g++ cmake
# ninja-build pkg-config libboost-dev libssl-dev liblua5.1-0-dev lua-cjson-dev
# lua-bitop-dev lua-cmsgpack libyaml-cpp-dev libgtest-dev, and for its
# step clang, clang-format, clang-tidy, libhiredis-dev or lcov.

set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
out=${MIMIR_CI_OUT:-$top/build-ci}
jobs=${MIMIR_CI_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}
step=${1:-}
if [ -z "${MIMIR_DANNAN_REPOSITORY:-}" ]; then
  if [ -d "$top/../dannan/.git" ]; then
    MIMIR_DANNAN_REPOSITORY=$top/../dannan
  else
    MIMIR_DANNAN_REPOSITORY=https://github.com/tomdionysus/dannan.git
  fi
fi

# Warnings are errors in every build but the sanitizers': instrumented, g++
# warns inside libstdc++ and Asio, not in Mimir.
configure() {
  dir="$1"
  shift
  cmake -S "$top" -B "$out/$dir" -G Ninja -DMIMIR_BUILD_TESTING=ON -DMIMIR_WARNINGS_AS_ERRORS=ON -DMIMIR_DANNAN_REPOSITORY="$MIMIR_DANNAN_REPOSITORY" "$@"
  cmake --build "$out/$dir" -j "$jobs"
}

case "$step" in
  check)
    cd "$top"
    missing=0
    for f in $(git ls-files '*.h' '*.cpp' '*.h.in'); do
      if ! head -3 "$f" | grep -qx '// Mimir - The Distributed Database'; then
        echo "no header block: $f"
        missing=1
      fi
    done
    [ "$missing" = 0 ]
    git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run -Werror
    ;;
  release)
    configure release -DCMAKE_BUILD_TYPE=Release
    "$out/release/mimir_tests"
    ;;
  clang)
    CXX=clang++ configure clang -DCMAKE_BUILD_TYPE=Release
    "$out/clang/mimir_tests"
    ;;
  asan)
    flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    configure asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMIMIR_WARNINGS_AS_ERRORS=OFF -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    ASAN_OPTIONS=detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1 "$out/asan/mimir_tests"
    ;;
  tsan)
    flags="-fsanitize=thread -fno-omit-frame-pointer"
    configure tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMIMIR_WARNINGS_AS_ERRORS=OFF -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" "$out/tsan/mimir_tests"
    ;;
  coverage)
    configure coverage -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage
    find "$out/coverage" -name '*.gcda' -delete
    (cd "$out/coverage" && ./mimir_tests)
    ignore=inconsistent,inconsistent,mismatch,negative,unused
    lcov --quiet --capture --directory "$out/coverage" --ignore-errors "$ignore" --output-file "$out/all.info"
    lcov --quiet --extract "$out/all.info" "$top/mimir/*" --ignore-errors "$ignore" --output-file "$out/library.info"
    lcov --quiet --remove "$out/library.info" "$top/mimir/test/*" "$top/mimir/fuzz/*" --ignore-errors "$ignore" --output-file "$out/coverage.info"
    rm "$out/all.info" "$out/library.info"
    lcov --summary "$out/coverage.info" --ignore-errors "$ignore"
    ;;
  client)
    configure client -DCMAKE_BUILD_TYPE=Release -DMIMIR_BUILD_TESTING=OFF -DMIMIR_BUILD_CLIENT_TESTS=ON
    "$out/client/mimir_client_tests"
    ;;
  tidy)
    # Configured, not built: clang-tidy needs only the compile database.
    tidy=$(command -v run-clang-tidy || command -v run-clang-tidy-19)
    cmake -S "$top" -B "$out/tidy" -G Ninja -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DCMAKE_BUILD_TYPE=Release -DMIMIR_BUILD_TESTING=ON \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DMIMIR_DANNAN_REPOSITORY="$MIMIR_DANNAN_REPOSITORY"
    ours="^$top/(mimir|tools)/"
    "$tidy" -p "$out/tidy" -j "$jobs" -quiet -checks='-*,bugprone-use-after-move' -warnings-as-errors='*' -header-filter="$ours" "$ours"
    ;;
  *)
    echo "usage: tools/ci.sh check|release|clang|asan|tsan|coverage|client|tidy" >&2
    exit 2
    ;;
esac
