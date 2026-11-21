# Mimir: why it exists, the stack, and where it stands

## Why it exists

Redis is the shared memory of a great many systems: caches, counters, queues, sessions,
rate limits and locks. A single Redis machine stops every one of them when it is
rebooted, filled or cut off. Redis's own answers to that each keep a leader. Replication
and Sentinel fail over to a replica. Redis Cluster gives every slot a primary. A primary
acknowledges a write before its replicas have it, so a failover can lose acknowledged
writes and hand one lock to two clients.

A store whose writes only merge has no leader, but it cannot keep the commands people
reach for Redis to get: two sides of a split would both win one `SET NX`, and one pop
would deliver an element twice. The maintainer's statement of the purpose: "This is the
purpose of Mimir. Otherwise I'd just use redis. they all need to work, with no
centralisation, in a soft multimaster cluster, using the underlying distributed stack."

Mimir is that store, written once for this stack:

- **No primary.** Every node takes every command for every key. There is no leader, no
  failover and no node whose loss stops the database.
- **Two classes of command.** A command whose answer depends on the value before it
  (`SET NX`, every pop and move, `MULTI`/`EXEC`, scripts) commits by agreement on a
  majority of its key's replicas, through Dannan's leaderless agreement, so a lock is
  never held twice. A command whose effects commute (`SET`, `HSET`, `SADD`, pushes) will
  be applied by any replica and merge, so it keeps working on both sides of a split;
  until that class is built, every write takes agreement.
- **All of Redis.** Every client library and tool that speaks Redis works unchanged.
  "Compatible" is defined by named gates: Valkey 8's documented behaviour, tested; a
  differential gate that sends the same commands to Mimir and to a Valkey 8 server and
  compares the replies byte for byte; and a real-client gate, hiredis against a
  three-node cluster.
- **On the cluster that is already there.** Membership, reachability, identity,
  placement, agreement and the wire between nodes are Dannan's; what must survive a node
  will be Dagda's. Mimir adds only what makes it a database.

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

- **[Dannan](https://github.com/tomdionysus/dannan)** is the network: reachability,
  routes and channels between nodes, membership, identity, placement, work classes,
  logging and the shared primitives, and the leaderless per-key agreement Mimir commits
  through. Mimir links it for its listener, crypto, configuration reader, logging and
  clocks.
- **[Dagda](https://github.com/tomdionysus/dagda)** is the store. Mimir's persistence
  will live there.
- **[Lugh](https://github.com/tomdionysus/lugh)** is the directory and sign-on. To Mimir
  it is one possible identity provider, through an optional plugin; Mimir itself never
  links Lugh.
- **[Lir](https://github.com/tomdionysus/lir)** is the event system. The Lugh plugin
  hears Lugh's principal changes as an MQTT client of Lir.

Hosted systems never address one another. Any system uses Mimir as it would any Redis
server.

## Where it stands

**Done.** Every Valkey type and command family over RESP2 and RESP3, transactions,
scripting and functions, pub/sub and keyspace notifications, and ACL with `AUTH`, each
checked against Valkey 8.1.1 by the differential gate. Every write, on every node, a
cluster of one included, commits through Dannan's leaderless agreement on a majority:

- single-key writes, with a retried commit answering as the first did;
- `MULTI`/`EXEC` with `WATCH`, blocking pops, and scripts, each over one key;
- reads served from what the node knows committed, which may be stale but is never
  later undone, and strict reads confirmed with a majority for a connection that asks
  with `CLIENT STRICT ON`;
- `PUBLISH` and keyspace notifications reaching every node's subscribers;
- a restarted node catching up before it votes;
- an expired key deleted by agreement;
- users and permissions, scripts and function libraries changed by agreement, so every
  node answers alike;
- `MIMIR NODES`, every node a client may use.

**Not yet** (the limits of this release are in the README's "Limits"):

- a write naming more than one key, which waits for Dannan's atomic commit across
  replica sets;
- placement: every node is in every key's replica set until shards move as nodes join
  and leave;
- `KEYS`, `SCAN` and `DBSIZE` across the cluster.

**Next**, in order (`TODO/ACTIVE.md`):

1. Merging writes and the rule at heal: a value agreed by a majority is never overturned
   by a merging write made on the minority side of a split.
2. Persistence in Dagda: `ephemeral`, `persisted`, `decided` and `replicated`
   acknowledgement levels, per command or per session.
3. Eviction that works whichever node a client talks to, then the Lugh plugin.
