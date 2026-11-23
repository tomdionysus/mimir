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
#   MIMIR_DIFF_NODES=3 tools/differential.sh   (each case against a cluster)
#
# With MIMIR_DIFF_NODES above 1, each case gets a fresh cluster of that many
# founders on loopback, and talks to one node of it, a different node from
# one case to the next: every command must work whichever node a client
# talks to. A cluster is ready when a write through each node commits (the
# key written and deleted in database 15, so no case sees it).
#
# Prints each case's result and, at the end, the count of differing
# commands. The full output is in <build directory>/differential.log
# (differential-<n>.log for a cluster of n).

set -u -o pipefail

build=${1:-build-ci/release}
tests=$build/mimir_tests
node_bin=$build/mimir-node
[ -x "$node_bin" ] || node_bin=$build/tools/mimir-node
valkey_port=${MIMIR_DIFF_VALKEY_PORT:-7379}
mimir_port=${MIMIR_DIFF_MIMIR_PORT:-7380}
nodes=${MIMIR_DIFF_NODES:-1}
log=$build/differential.log
[ "$nodes" -gt 1 ] && log=$build/differential-$nodes.log

for f in "$tests" "$node_bin"; do
  [ -x "$f" ] || { echo "not found: $f" >&2; exit 2; }
done
command -v valkey-server >/dev/null || { echo "valkey-server is not installed" >&2; exit 2; }

valkey_started=0
if ! valkey-cli -p "$valkey_port" PING >/dev/null 2>&1; then
  LC_ALL=C valkey-server --port "$valkey_port" --bind 127.0.0.1 --save "" --appendonly no --daemonize yes >/dev/null
  valkey_started=1
fi
pids=""
work=$(mktemp -d "${TMPDIR:-/tmp}/mimir-differential.XXXXXX")
stop_nodes() {
  for pid in $pids; do kill "$pid" 2>/dev/null; done
  for pid in $pids; do wait "$pid" 2>/dev/null; done
  pids=""
}
finish() {
  stop_nodes
  rm -rf "$work"
  [ "$valkey_started" -eq 1 ] && valkey-cli -p "$valkey_port" shutdown nosave >/dev/null 2>&1
}
trap finish EXIT

# A fresh cluster of $nodes founders: client ports mimir_port and up, cluster
# ports from mimir_port + 100. Returns non-zero if it never took writes.
start_cluster() {
  rm -rf "$work/c"
  mkdir -p "$work/c"
  head -c 32 /dev/urandom >"$work/c/cluster.key"
  printf 'log:\n  level: ERROR\n' >"$work/c/config.yaml"
  ids=""
  for i in $(seq 0 $((nodes - 1))); do
    id=$("$node_bin" --config "$work/c/config.yaml" --cluster-state "$work/c/node-$i" --init-state 2>/dev/null | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
    [ -n "$id" ] || return 1
    ids="$ids --genesis $id"
  done
  for i in $(seq 0 $((nodes - 1))); do
    others=""
    for j in $(seq 0 $((nodes - 1))); do
      [ "$j" -ne "$i" ] && others="$others --bootstrap 127.0.0.1:$((mimir_port + 100 + j))"
    done
    # shellcheck disable=SC2086
    "$node_bin" --config "$work/c/config.yaml" --cluster-state "$work/c/node-$i" --listen 127.0.0.1 --port $((mimir_port + i)) --databases 16 \
      --auth-plaintext --cluster-key "$work/c/cluster.key" --cluster-listen 127.0.0.1 --cluster-advertise 127.0.0.1 \
      --cluster-port $((mimir_port + 100 + i)) --cluster-name "node-$i" $ids $others >"$work/c/node-$i.log" 2>&1 &
    pids="$pids $!"
  done
  for i in $(seq 0 $((nodes - 1))); do
    ready=0
    for _ in $(seq 300); do
      if [ "$(valkey-cli -p $((mimir_port + i)) -n 15 SET differential:ready 1 2>/dev/null)" = OK ]; then
        valkey-cli -p $((mimir_port + i)) -n 15 DEL differential:ready >/dev/null 2>&1
        ready=1
        break
      fi
      sleep 0.2
    done
    [ "$ready" -eq 1 ] || return 1
  done
}

cases=$("$tests" --gtest_list_tests --gtest_filter='Differential.*' | while read -r line; do
  case "$line" in
    Differential.*) ;;
    *) [ -n "$line" ] && echo "Differential.${line%% *}" ;;
  esac
done)

: >"$log"
passed=0
failed=0
index=0
for name in $cases; do
  valkey-cli -p "$valkey_port" FLUSHALL >/dev/null
  port=$mimir_port
  if [ "$nodes" -gt 1 ]; then
    # Each case talks to the next node in turn.
    port=$((mimir_port + index % nodes))
    if ! start_cluster; then
      failed=$((failed + 1))
      echo "FAILED $name (the cluster never took writes)"
      { echo "[ CLUSTER ] $name: the cluster never took writes"; cat "$work"/c/node-*.log; } >>"$log"
      stop_nodes
      index=$((index + 1))
      continue
    fi
  else
    "$node_bin" --listen 127.0.0.1 --port "$mimir_port" --databases 16 --auth-plaintext --seconds 600 >/dev/null 2>&1 &
    pids=$!
    for _ in $(seq 50); do
      valkey-cli -p "$mimir_port" PING >/dev/null 2>&1 && break
      sleep 0.1
    done
  fi
  if MIMIR_DIFF_MIMIR_PORT=$port MIMIR_DIFF_VALKEY_PORT=$valkey_port "$tests" --gtest_filter="$name" >>"$log" 2>&1; then
    passed=$((passed + 1))
    echo "passed $name (node $((port - mimir_port)))"
  else
    failed=$((failed + 1))
    echo "FAILED $name (node $((port - mimir_port)))"
  fi
  stop_nodes
  index=$((index + 1))
done

# Each differing reply is one GoogleTest "Failure"; those refused by ruling 35
# (several keys) or ruling 36 (a script's undeclared key) are counted apart.
differing=$(grep -c ': Failure$' "$log" || true)
ruling35=$(grep -c 'Mimir refuses until Dannan' "$log" || true)
ruling36=$(grep -c 'must declare it' "$log" || true)
echo "cases: $passed passed, $failed failed"
echo "differing replies: $differing ($ruling35 ruling 35 refusals, $ruling36 undeclared script keys; log: $log)"
