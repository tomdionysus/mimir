#!/usr/bin/env bash
#
# Mimir - The Distributed Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The differential gate, one case at a time: each Differential.* case runs
# against a freshly started mimir-node, since FLUSHALL is refused until
# Dannan's atomic commit across replica sets (ruling 35) and the cases cannot
# share one node. Valkey 8 is started once. Every node this script starts is stopped by its own PID,
# never by name: the build servers run mimir.service.
#
#   tools/differential.sh [build directory]    (default build-ci/release)
#
# Prints each case's result and, at the end, the count of differing
# commands. The full output is in <build directory>/differential.log.

set -u -o pipefail

build=${1:-build-ci/release}
tests=$build/mimir_tests
node_bin=$build/mimir-node
[ -x "$node_bin" ] || node_bin=$build/tools/mimir-node
valkey_port=${MIMIR_DIFF_VALKEY_PORT:-7379}
mimir_port=${MIMIR_DIFF_MIMIR_PORT:-7380}
log=$build/differential.log

for f in "$tests" "$node_bin"; do
  [ -x "$f" ] || { echo "not found: $f" >&2; exit 2; }
done
command -v valkey-server >/dev/null || { echo "valkey-server is not installed" >&2; exit 2; }

valkey_started=0
if ! valkey-cli -p "$valkey_port" PING >/dev/null 2>&1; then
  LC_ALL=C valkey-server --port "$valkey_port" --bind 127.0.0.1 --save "" --appendonly no --daemonize yes >/dev/null
  valkey_started=1
fi
node=0
finish() {
  [ "$node" -ne 0 ] && kill "$node" 2>/dev/null && wait "$node" 2>/dev/null
  [ "$valkey_started" -eq 1 ] && valkey-cli -p "$valkey_port" shutdown nosave >/dev/null 2>&1
}
trap finish EXIT

cases=$("$tests" --gtest_list_tests --gtest_filter='Differential.*' | while read -r line; do
  case "$line" in
    Differential.*) ;;
    *) [ -n "$line" ] && echo "Differential.${line%% *}" ;;
  esac
done)

: >"$log"
passed=0
failed=0
for name in $cases; do
  valkey-cli -p "$valkey_port" FLUSHALL >/dev/null
  "$node_bin" --listen 127.0.0.1 --port "$mimir_port" --databases 16 --auth-plaintext --seconds 600 >/dev/null 2>&1 &
  node=$!
  for _ in $(seq 50); do
    valkey-cli -p "$mimir_port" PING >/dev/null 2>&1 && break
    sleep 0.1
  done
  if MIMIR_DIFF_MIMIR_PORT=$mimir_port MIMIR_DIFF_VALKEY_PORT=$valkey_port "$tests" --gtest_filter="$name" >>"$log" 2>&1; then
    passed=$((passed + 1))
    echo "passed $name"
  else
    failed=$((failed + 1))
    echo "FAILED $name"
  fi
  kill "$node" 2>/dev/null
  wait "$node" 2>/dev/null
  node=0
done

# Each differing reply is one GoogleTest "Failure"; those refused by ruling 35
# (several keys) or ruling 36 (a script's undeclared key) are counted apart.
differing=$(grep -c ': Failure$' "$log" || true)
ruling35=$(grep -c 'Mimir refuses until Dannan' "$log" || true)
ruling36=$(grep -c 'must declare it' "$log" || true)
echo "cases: $passed passed, $failed failed"
echo "differing replies: $differing ($ruling35 ruling 35 refusals, $ruling36 undeclared script keys; log: $log)"
