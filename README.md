# Mimir - The Distributed Database

[![Release](https://img.shields.io/github/v/tag/tomdionysus/mimir?sort=semver&label=release)](https://github.com/tomdionysus/mimir/tags)
[![CI](https://github.com/tomdionysus/mimir/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/mimir/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/mimir/branch/develop/graph/badge.svg)](https://codecov.io/gh/tomdionysus/mimir)
[![CodeQL](https://github.com/tomdionysus/mimir/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/mimir/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](CMakeLists.txt)
[![Platform: Linux x86-64 and arm64](https://img.shields.io/badge/platform-Linux%20x86--64%20%7C%20arm64-lightgrey.svg)](.github/workflows/ci.yml)
[![Protocol: RESP2 and RESP3](https://img.shields.io/badge/protocol-RESP2%20%7C%20RESP3-informational.svg)](#use-it-from-a-client)
[![Compatibility: Valkey 8](https://img.shields.io/badge/compatible-Valkey%208-informational.svg)](docs/overview.md)
[![Built on Dannan](https://img.shields.io/badge/built%20on-Dannan%200.9.2-informational.svg)](https://github.com/tomdionysus/dannan)

Mimir is a distributed database for a cluster of trusted machines that speaks the Redis
protocol. A client connects to any node, over RESP2 or RESP3, and every command works,
whichever node it talks to. Every write commits by agreement on a majority of the
cluster, with no leader and no central coordinator, so a lock is never held twice and a
pop never delivers an element twice. Valkey 8's command documentation is the
specification; nothing of Redis or Valkey is copied.

It runs on [Dannan](https://github.com/tomdionysus/dannan), the network layer, and will
persist in [Dagda](https://github.com/tomdionysus/dagda), the store. Why it exists, how
it fits the stack and what comes next: [docs/overview.md](docs/overview.md).

## Install

### From the package

Mimir is packaged for Debian 13 (trixie) on x86-64 and arm64 as `mimir`, which depends
on Dannan's `libdannan` package and Debian's Lua libraries
([docs/packaging.md](docs/packaging.md)). With both packages in hand:

```
sudo apt install ./libdannan0.9_*.deb ./mimir_*.deb
```

The package installs `/usr/bin/mimir-node`, the manual page `mimir-node(1)`,
`/etc/mimir/config.yaml` (a cluster of one, on loopback), and `mimir.service`, installed
disabled. It makes the `mimir` user, `/etc/mimir/secrets.yaml` for the users,
`/etc/mimir/cluster.key` and `/var/lib/mimir`. Review the settings, then:

```
sudo systemctl enable --now mimir
redis-cli -p 6379 SET greeting hello
redis-cli -p 6379 GET greeting
```

### From source

Debian 13, or any system with GCC 14 or Clang 19, CMake 3.24, Boost 1.83, OpenSSL 3,
Lua 5.1 with lua-cjson, lua-bitop and lua-cmsgpack, and yaml-cpp. CMake uses an
installed `libdannan-dev` of the pinned minor, or else fetches Dannan at its tag from
`-DMIMIR_DANNAN_REPOSITORY` (a sibling checkout by default):

```
apt-get install g++ cmake ninja-build pkg-config libboost-dev libssl-dev liblua5.1-0-dev \
  lua-cjson-dev lua-bitop-dev lua-cmsgpack libyaml-cpp-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMIMIR_DANNAN_REPOSITORY=https://github.com/tomdionysus/dannan.git
cmake --build build
sudo cmake --install build
```

Building and running the tests, and the build options: [CONTRIBUTING.md](CONTRIBUTING.md).

## Run a node

`mimir-node` is one node. It reports what it does as one JSON object per line, first a
`listening` event with its ports and its node id.

```
mimir-node --port 6379
```

Started like this, with no state directory, it is a throwaway cluster of one: its writes
go through agreement as a larger cluster's do, and nothing outside the host can join it.
The packaged service is a cluster of one with a state directory, so its node id lasts.

## Run a cluster

Every node of a cluster has the same cluster key file, a state directory of its own, the
ids of the founding nodes, and the same shard count, fixed for the cluster's life. Make
each founder's state and id first; each command prints the node's id:

```
sudo -u mimir mimir-node --config /etc/mimir/config.yaml --init-state
{"event":"initialised","id":"0123456789abcdef0123456789abcdef"}
```

Copy one node's `/etc/mimir/cluster.key` to every node (mode 600, owned by `mimir`),
then give each node the founders' ids, the others' cluster ports, and addresses the
other nodes can reach, in `/etc/mimir/config.yaml`:

```yaml
listen:
  host: 0.0.0.0
  port: 6379
cluster:
  key: /etc/mimir/cluster.key
  state: /var/lib/mimir
  listen: 0.0.0.0
  port: 16379
  advertise: node-a.example.com
  genesis:
    - 0123456789abcdef0123456789abcdef
    - 123456789abcdef0123456789abcdef0
    - 23456789abcdef0123456789abcdef01
  bootstrap:
    - node-b.example.com:16379
    - node-c.example.com:16379
  shards: 256
```

Then start the service on every node. A cluster takes writes while a majority of its
nodes are up; a node that restarts catches up from the others before it votes.

## Configure

Every setting is a key in a YAML file, a `MIMIR_*` variable and a flag, read by the
stack's one settings reader (Dannan's): the file (`--config PATH`, `$MIMIR_CONFIG`, or
`/usr/local/etc/mimir/config.yaml` when it exists), then `secrets.yaml` beside it, then
the environment, then the flags, each overriding the one before. `--port 6379` is
`listen: {port: 6379}` and `MIMIR_PORT`. A value that cannot be used is refused at
start, naming the file, variable or flag that gave it. Every setting is described in
`mimir-node(1)` ([docs/man/mimir-node.1](docs/man/mimir-node.1)).

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

### Users and AUTH

The node starts with the users in its settings, each a line of a Valkey ACL file, kept
in `secrets.yaml`:

```yaml
acl:
  users:
    - user default off
    - user app on >secret ~app:* &app:* +@all -@dangerous
```

`ACL SETUSER` and `ACL DELUSER` change them while it runs, by agreement, so every node
answers alike. With no users configured, the `default` user may do everything without a
password, as Valkey's does.

A password never crosses the network in clear: `AUTH` and `HELLO ... AUTH` are refused
on a plaintext connection unless `auth: {plaintext: true}` (`--auth-plaintext`) is set.

### TLS

Give the node a certificate chain, a key and a TLS port; name a CA bundle to require
client certificates:

```
mimir-node --port 6379 --tls-port 6380 --tls-certificate node.pem --tls-key node.key \
  [--tls-client-ca clients-ca.pem]
redis-cli -p 6380 --tls --cacert ca.pem --user app --pass secret PING
```

## Use it from a client

Any Redis client works unchanged, over RESP2 or RESP3 (`HELLO 3`), with AUTH,
`CLIENT SETNAME` and `CLIENT SETINFO`. Point it at any node; there are no `MOVED` or
`ASK` redirections.

- **Every node a client may use**: `MIMIR NODES` lists each node's id, name, host, port,
  TLS port and whether it is the one answering, so a client whose node goes away can
  connect to another.
- **Fresh reads**: a plain read answers from what the node knows committed, which may
  trail a write just made on another node but is never later undone. `CLIENT STRICT ON`
  confirms every read on the connection with a majority, and is refused on the minority
  side of a split.
- **Splits**: only the side holding a majority commits writes; the other side refuses
  them. Nothing is merged and nothing is undone.

## Limits

- **Everything is in memory.** A cluster keeps what a majority of its nodes hold; a
  cluster of one starts empty after a restart. Persistence in Dagda is next.
- **A write naming more than one key is refused**, with an error saying so: `MSET`,
  `RENAME`, `LMOVE`, `SMOVE`, the `STORE` commands, `FLUSHALL` and `FLUSHDB`, and
  transactions and scripts that write several keys. They wait for Dannan's atomic commit
  across replica sets. Reads of several keys (`MGET`, `SUNION`, ...) work.
- **A script writes only the keys it declares**, as Valkey documents ("all keys must be
  passed explicitly").
- `KEYS`, `SCAN` and `DBSIZE` answer for the node's own view of the cluster.

## Code of conduct

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md); see also
[CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
