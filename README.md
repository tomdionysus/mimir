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

Version 0.1.0, in development. The single-node stage is complete, served by
`mimir-node`: RESP2 and RESP3, every Valkey type and command family,
transactions, pub/sub, scripting and functions, keyspace notifications, and
Valkey's ACL with AUTH. The cluster (agreement through Dannan, merging writes)
and persistence in Dagda follow, in the order `TODO/ACTIVE.md` gives.

## Users and AUTH

Valkey's ACL: users, their passwords and their permissions over commands,
keys and channels, changed with `ACL SETUSER`. The node starts with the users
in its settings, each a line of a Valkey ACL file, best kept in `secrets.yaml`
beside the configuration:

```
acl:
  users:
    - user default off
    - user app on >secret ~app:* &app:* +@all -@dangerous
```

A password never crosses the network in clear: `AUTH` and `HELLO ... AUTH` are
refused on a plaintext connection unless `auth: {plaintext: true}` (or
`--auth-plaintext`) is set. Use the TLS port (`--tls-port`).

Compatibility is Valkey 8's documented behaviour, tested, with a differential
gate against a Valkey 8 server.

## Building

Debian 13 (or any system with GCC 14 or Clang 19, CMake 3.24, Boost 1.83,
OpenSSL 3, Lua 5.1, yaml-cpp and GoogleTest):

```
apt-get install g++ cmake ninja-build pkg-config libboost-dev libssl-dev liblua5.1-0-dev lua-cjson-dev lua-bitop-dev lua-cmsgpack libyaml-cpp-dev libgtest-dev
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
