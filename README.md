# Mimir - The Distributed Database

[![CI](https://github.com/tomdionysus/mimir/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/mimir/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/mimir/branch/develop/graph/badge.svg)](https://codecov.io/gh/tomdionysus/mimir)
[![CodeQL](https://github.com/tomdionysus/mimir/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/mimir/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Mimir is a distributed database for a cluster of trusted machines that speaks the Redis
protocol. It is a C++20 library and a node binary that turn a set of nodes into one
Redis-compatible store. A client connects to any node, over RESP2 or RESP3, and every
command works, whichever node it talks to. That includes locks, compare-and-set and
exactly-once pops, with no leader and no central coordinator. It runs on
[Dannan](https://github.com/tomdionysus/dannan), the network, and will persist in
[Dagda](https://github.com/tomdionysus/dagda), the store.

Mimir is Redis compatible, not Redis based: Valkey 8's command documentation is the
specification, and nothing of Redis or Valkey is copied.

## Why it exists

Redis is the shared memory of a great many systems: caches, counters, queues, sessions,
rate limits and locks. A single Redis machine stops every one of them when it is
rebooted, filled or cut off. Redis's own answers to that each keep a leader. Replication
and Sentinel fail over to a replica. Redis Cluster gives every slot a primary. A primary
acknowledges a write before its replicas have it, so a failover can lose acknowledged
writes and hand one lock to two clients.

A store whose writes only merge has no leader, but it cannot keep the commands people
reach for Redis to get: two sides of a split would both win one `SET NX`, and one pop
would deliver an element twice. Tom's statement of the purpose: "This is the purpose of
Mimir. Otherwise I'd just use redis. they all need to work, with no centralisation, in a
soft multimaster cluster, using the underlying distributed stack."

Mimir is that store, written once for this stack:

- **No primary.** Every node takes every command for every key. There is no leader, no
  failover and no node whose loss stops the database.
- **Two classes of command.** A command whose answer depends on the value before it
  (`SET NX`, every pop and move, `MULTI`/`EXEC`, scripts) commits by agreement on a
  majority of its key's replicas, through Dannan's leaderless agreement, so a lock is
  never held twice. A command whose effects commute (`SET`, `HSET`, `SADD`, pushes) is
  applied by any replica and merges, so it keeps working on both sides of a split.
- **All of Redis.** Every client library and tool that speaks Redis works unchanged.
  "Compatible" is defined by named gates: Valkey 8's documented behaviour, tested, and a
  differential gate that sends the same commands to Mimir and to a Valkey 8 server and
  compares the replies byte for byte.
- **On the cluster that is already there.** Membership, reachability, identity,
  placement, agreement and the wire between nodes are Dannan's; what must survive a node
  will be Dagda's. Mimir adds only what makes it a database.

## What it does

- **RESP2 and RESP3.** A request parser written here for untrusted input: every length
  bounded before anything is allocated, tighter bounds before a client authenticates,
  Valkey's protocol errors word for word, and a fuzz target. Pipelined replies go out in
  one write.
- **Every Valkey type and command family**: strings, keys and expiry, lists, hashes,
  sets, sorted sets, bitmaps and bitfields, HyperLogLog, streams with consumer groups,
  geospatial indexes, `SORT`, `LCS`, blocking pops and moves, and configurable databases
  (`SELECT`, one by default).
- **Transactions**: `MULTI`, `EXEC`, `DISCARD` and `WATCH`, the queue run whole.
- **Scripting**: `EVAL`, `EVALSHA`, `SCRIPT`, and `FUNCTION` with `FCALL`, on Lua 5.1
  with Valkey's bundled libraries (`cjson`, `cmsgpack`, `bit`, `struct`) and its
  read-only sandbox.
- **Publish and subscribe**: channels, patterns and shard channels, and keyspace
  notifications.
- **Users and permissions**: Valkey's ACL, with users, passwords, and permissions over
  commands, categories, keys and channels, changed with `ACL SETUSER`, and `AUTH`. A
  password is never taken over a plaintext connection unless the node is told to allow
  it.
- **Server commands**: `INFO`, `CLIENT`, `CONFIG`, `COMMAND`, `OBJECT`, `DUMP` and
  `RESTORE` in Valkey's own serialisation, and the rest a client expects to find.

## The stack

Mimir is one of a stack of projects, each its own library and repository, written to
the same conventions. No code is duplicated between them: a piece needed by more than
one lives in exactly one.

```
   Lugh        Lir        Mimir        Macha, ...      hosted systems
     |          |           |
   Dagda: the store  (domains, sealing, objects, heads, leases)
     |
   Dannan: the network  (reachability, routes, channels, membership,
                         identity, placement, agreement, status)
```

- **[Dannan](https://github.com/tomdionysus/dannan)** is the network: reachability, routes
  and channels between nodes, membership, identity, placement, work classes, logging and
  the shared primitives, and from 0.9 the leaderless per-key agreement Mimir's agreement
  class commits through. Mimir links it for its listener, crypto, configuration reader,
  logging and clocks.
- **[Dagda](https://github.com/tomdionysus/dagda)** is the store. Mimir's `persisted` and
  `replicated` durability levels will write there.
- **[Lugh](https://github.com/tomdionysus/lugh)** is the directory and sign-on. To Mimir
  it is one possible identity provider, through an optional plugin; Mimir itself never
  links Lugh.
- **[Lir](https://github.com/tomdionysus/lir)** is the event system. The Lugh plugin hears
  Lugh's principal changes as an MQTT client of Lir.

Hosted systems never address one another. Any system uses Mimir as it would any Redis
server.

## How to use it

### Build

Debian 13, or any system with GCC 14 or Clang 19, CMake 3.24, Boost 1.83, OpenSSL 3,
Lua 5.1 with lua-cjson, lua-bitop and lua-cmsgpack, yaml-cpp and GoogleTest. Dannan is
fetched from `../dannan` at the pinned release (0.8.4), so clone it beside this
repository.

```
apt-get install g++ cmake ninja-build pkg-config libboost-dev libssl-dev liblua5.1-0-dev \
  lua-cjson-dev lua-bitop-dev lua-cmsgpack libyaml-cpp-dev libgtest-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMIMIR_BUILD_TESTING=ON
cmake --build build
./build/mimir_tests
```

Build options: `MIMIR_BUILD_TESTING`, `MIMIR_BUILD_FUZZING` (clang, libFuzzer targets for
the RESP parser and `RESTORE`), `MIMIR_WARNINGS_AS_ERRORS`.

### Run a node

`mimir-node` is one node. It reports what it does as one JSON object per line.

```
./build/mimir-node --port 6379
#   {"event":"listening","port":6379,"tls_port":0}
redis-cli -p 6379 SET greeting hello
redis-cli -p 6379 GET greeting
```

### Configure

Every flag is also a key in a YAML file and a `MIMIR_*` variable, read by the stack's one
settings reader (Dannan's): the file (`--config PATH`, `$MIMIR_CONFIG`, or
`/usr/local/etc/mimir/config.yaml` when it exists), then `secrets.yaml` beside it, then
the environment, then the flags, each overriding the one before. `--port 6379` is
`listen: {port: 6379}` and `MIMIR_PORT`:

```yaml
listen:
  host: 0.0.0.0
  port: 6379
tls:
  port: 6380
  certificate: /etc/mimir/node.pem
  key: /etc/mimir/node.key
databases: 16
log:
  level: INFO
  format: json
```

A value that cannot be used is refused at start, naming the file, variable or flag that
gave it. `tools/mimir_node.cpp`, `reader()`, lists every key.

### Users and AUTH

The node starts with the users in its settings, each a line of a Valkey ACL file, best
kept in `secrets.yaml`:

```yaml
acl:
  users:
    - user default off
    - user app on >secret ~app:* &app:* +@all -@dangerous
```

`ACL SETUSER` and `ACL DELUSER` change them while it runs (in the cluster, by agreement,
so every node answers alike). With no users configured, the `default` user may do
everything without a password, as Valkey's does.

A password never crosses the network in clear: `AUTH` and `HELLO ... AUTH` are refused on
a plaintext connection unless `auth: {plaintext: true}` (`--auth-plaintext`) is set.

### TLS

Give the node a certificate chain, a key and a TLS port; name a CA bundle to require
client certificates:

```
./build/mimir-node --port 6379 --tls-port 6380 --tls-certificate node.pem --tls-key node.key \
  [--tls-client-ca clients-ca.pem]
redis-cli -p 6380 --tls --cacert ca.pem --user app --pass secret PING
```

### The differential gate

The tests compare Mimir with a Valkey 8 server when both are running on loopback:

```
LC_ALL=C valkey-server --port 7379 --bind 127.0.0.1 --save "" --appendonly no --daemonize yes
./build/mimir-node --listen 127.0.0.1 --port 7380 --databases 16 --auth-plaintext --seconds 200 &
MIMIR_DIFF_MIMIR_PORT=7380 MIMIR_DIFF_VALKEY_PORT=7379 ./build/mimir_tests --gtest_filter='Differential.*'
```

## Repository layout

```
mimir/resp/       the RESP2 and RESP3 request parser and reply writer
mimir/keyspace/   values and expiry: hash slots, glob patterns, the types' own
                  structures, geohashes, HyperLogLog, streams, RDB serialisation
mimir/command/    the command table, the commands, the database, scripting,
                  pub/sub and ACL
mimir/server/     connections over Dannan's listener
mimir/test/       GoogleTest, one binary (mimir_tests), with the differential gate
mimir/fuzz/       libFuzzer targets for untrusted bytes
tools/            mimir-node, and the CI script
docs/             the principles and laws shared with the stack
TODO/             now and next (ACTIVE.md), the design, and what shipped (COMPLETED.md)
```

Conventions, shared with the stack: [CONTRIBUTING.md](CONTRIBUTING.md).

## Status

Version 0.1.0, in development; no release is cut yet. The single-node stage is complete
and gated: every Valkey type and command family over RESP2 and RESP3, transactions,
scripting and functions, pub/sub and keyspace notifications, and ACL with `AUTH`, each
checked against Valkey 8.1.1 by the differential gate (277 tests, and 32 differential
cases).

What follows, in order (`TODO/ACTIVE.md`):

1. The cluster by agreement: every key on three replicas, agreement-class commands
   committed through Dannan's leaderless agreement, which ships in Dannan 0.9.0.
   Multi-key commands across slots commit atomically (Tom's ruling), by an atomic commit
   across replica sets proposed to Dannan.
2. Merging writes and the rule at heal: a value agreed by a majority is never overturned
   by a merging write made on the minority side of a split.
3. Persistence in Dagda: `ephemeral`, `persisted`, `decided` and `replicated`
   acknowledgement levels, per command or per session.
4. Eviction that works whichever node a client talks to, then the Lugh plugin.

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
