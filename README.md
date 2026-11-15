# Mimir

[![CI](https://github.com/tomdionysus/mimir/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/mimir/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/mimir/branch/develop/graph/badge.svg)](https://codecov.io/gh/tomdionysus/mimir)
[![CodeQL](https://github.com/tomdionysus/mimir/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/mimir/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

The distributed database: a Redis-compatible, multi-master, leaderless
datastore. Clients speak RESP2 or RESP3 to any node, and every command works
whichever node they talk to, locks, compare-and-set and exactly-once pops
included, with no leader and no central coordinator. Mimir is a hosted system
on Dannan, the stack's network layer, and persists in Dagda, the stack's store.

## Status

Version 0.1.0, in development. The single-node stage is under way: the RESP2
and RESP3 protocol, the keyspace with expiry, and the string, key and
connection commands, served by `mimir-node`. The other types, the cluster
(agreement through Dannan, merging writes) and persistence in Dagda follow, in
the order `TODO/ACTIVE.md` gives.

Compatibility is Valkey 8's documented behaviour, tested, with a differential
gate against a Valkey 8 server.

## Building

Debian 13 (or any system with GCC 14 or Clang 19, CMake 3.24, Boost 1.83,
OpenSSL 3, yaml-cpp and GoogleTest):

```
apt-get install g++ cmake ninja-build libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMIMIR_BUILD_TESTING=ON
cmake --build build
./build/mimir_tests
./build/mimir-node --port 6379
```

Build options: `MIMIR_BUILD_TESTING`, `MIMIR_BUILD_FUZZING` (clang),
`MIMIR_WARNINGS_AS_ERRORS`. Dannan is fetched at its pinned tag.

## Roadmap

1. One node: every Valkey 8 type and command, RESP2 and RESP3.
2. The cluster by agreement: leaderless consensus per key through Dannan.
3. Merging writes, for the commands that can merge.
4. Persistence in Dagda: three durability levels per command or session.
5. Eviction, and authentication through Lugh.

## Licence

GPLv3. See [LICENSE](LICENSE).
