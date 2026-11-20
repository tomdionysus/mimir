# Mimir: design (2026-11-15, rulings to 2026-11-18)

Mimir is a Redis-compatible, multi-master, leaderless distributed database: a
hosted system on Dannan, persisting in Dagda, written new from this design.
Tom, 2026-11-15: "Mimir should be Redis compatible, not redis based", and of the
commands a merge-only store cannot keep (locks, compare-and-set, exactly-once
pops): "This is the purpose of Mimir. Otherwise I'd just use redis. they all
need to work, with no centralisation, in a soft multimaster cluster, using the
underlying distributed stack."

Written by the Mimir session from Tom's rulings in conversation and the answers
of the Dannan, Dagda, Lir and Lugh sessions (2026-11-15). Each statement is
marked:

- **Ruled**: Tom's decision, dated where it is later than 2026-11-15.
- **Agreed**: another session's answer about its own project, not disputed.
- **Proposed**: the Mimir session's recommendation. Nothing proposed is built
  until Tom rules and the owning session has reviewed it.

The rulings are the authority. The sections after them were written on
2026-11-15, before the cluster stage was built; where a later ruling changed
one, the section says so and names the ruling. What is built, and what is
next, is `TODO/ACTIVE.md`.

## Ruled (Tom)

1. **Every Redis command works, correctly, from any node, with no leader.**
   A merge-only store (two winners of one `SET NX` in a split) is not Mimir.
2. **Soft multimaster is a per-command choice.** Commands that need agreement
   need a majority of the key's replicas; commands that can merge keep working
   on both sides of a split and merge after.
3. **Read-modify-write commands (`INCR`, `HINCRBY`, `APPEND`, `GETSET`, `SET`
   with `GET`) are configurable**: agreement or merge.
4. **An agreed value wins at heal.** A value committed by a majority is never
   overturned by a merging write made on the minority side during the split.
5. **Reads on a minority side are served, possibly stale; a client may ask for
   a strict read**, which is confirmed with a majority and refused without one.
6. **The agreement mechanism (leaderless consensus per key) is built in
   Dannan**, for Mimir and for whoever else needs it.
7. **Three durability levels, per command or per session** (named by ruling
   24): `ephemeral`, written to memory, acknowledged at once; `persisted`,
   written to local disk, then acknowledged; `replicated`, acknowledged once a
   majority of the key's replicas have it on disk (Tom chose "Majority is enough", described as "acknowledged once a
   majority have it on disk; the rest catch up behind").
8. **The hybrid logical clock lives in Dannan, and Mimir waits for it**; Mimir
   writes no temporary copy.
9. **Eviction works properly whichever node a client talks to.**
10. **The compatibility reference is Valkey 8.**
11. **Tagline: "Mimir - The Distributed Database".**
12. **`AUTH` is accepted only over TLS when the Lugh plugin is on.** With the
    built-in user list, plaintext `AUTH` is off by default and a setting turns
    it on.
13. **Configuration is YAML.** The bound on a missing Lugh heartbeat is a
    setting.
14. **Lugh publishes principal changes over Lir** (Tom to the Lugh session);
    Mimir does not poll.

15. **The shard count is configurable, 256 by default.**
16. **An agreement command observes every write acknowledged at
    `replicated`.** A plain write at `ephemeral` or `persisted` may be ordered
    before it and lose; a client that needs them ordered uses `replicated` for
    the plain writes.
17. **Until Dagda's log exists, `persisted` and `replicated` are refused**, with an error
    saying persistence is not available.
18. **Lugh is asked for a narrow scope** answering only whether a named
    principal exists, is enabled, and its version; Mimir holds that, not
    `lugh:directory.read`.

19. **Anything that can use agreement waits for Dannan's agreement
    primitive** (Tom to Dannan, relayed 2026-11-15: "consensus/agreement is a
    huge part of higher order behaviour"). Mimir builds no consensus, no lease
    standing in for it and no workaround meanwhile; stage 3 has no interim path.

20. **Scripting uses Debian's liblua5.1** (EVAL, EVALSHA, SCRIPT, FUNCTION,
    FCALL), linked as a system package, not vendored.
21. **The number of databases is configurable**: one by default, more if
    configured, fixed for the cluster's life as the shard count is.
22. **Multi-key commands over several slots are allowed, atomic across
    slots** (MGET, MSET, DEL, the STOREs, MULTI/EXEC): in the cluster stage
    this needs a cross-shard commit on Dannan's agreement.
23. **An agreement command decides, then answers, at every level**: the
    level governs durability only (memory, local disk, majority on disk), so
    for an agreement command `ephemeral` gives what `decided` names, and a
    lock or a popped element is never handed out twice. (Kvasir's levels,
    ruled for SQL, differ.)
24. **The acknowledgement levels are named, not numbered, one standard for the
    group** (Tom to Kvasir, relayed 2026-11-15; the standard is
    `../kvasir/TODO/2026-11-15-ack-levels-standard.md`): `ephemeral` (was 0,
    answered at once), `persisted` (was 1, the node's own disk), `decided` (a
    majority has agreed the write, in memory, disks after) and `replicated`
    (was 2, a majority of the replicas on disk), ordered so. The setting is
    `ack_level` everywhere: the YAML key, the session setting and the
    per-command form; code, wire, Status, logs and documents use the names.
    Rulings 7, 16, 17 and 23 are written in these names below.
25. **Scripting's bundled libraries as Valkey's** (2026-11-15): `cjson`,
    `cmsgpack` and `bit` from Debian's packages (lua-cjson, lua-cmsgpack,
    lua-bitop), linked as system packages as ruling 20's Lua is; `struct`,
    which Debian does not package, written here.
26. **No root except at genesis** (Tom to Dannan, relayed 2026-11-15: "No
    root except at genesis. A root may start with only one node."). Mimir's
    shards are Dannan's fixed slots: each is touched at founding on the
    genesis set (possibly one node), its replica set then lives in its own
    acceptors and moves only by Dannan's transfer(), proposed by placement as
    nodes join. There is no shard-map register and no root role. The slot
    count travels in the cluster's identity beside the genesis members; a
    node configured with another is refused at join. Dannan's advert of the
    slots a node serves is ceil(count / 8) bytes, at most 1 KiB, so the
    shard count is capped at 8192 (ruling 15's default stays 256).
27. **ACL users are cluster state, by agreement** (2026-11-15): the YAML user
    list seeds them at genesis; ACL SETUSER and DELUSER then commit by
    agreement like any agreement command, so every node answers alike.
    Passwords are kept as SHA-256, as Valkey's ACL LIST shows them. On one
    node, in memory, seeded from YAML.
28. **Plaintext AUTH refused unless enabled** (2026-11-15, ruling 12 made
    exact): on a plaintext connection AUTH and HELLO's AUTH answer an error
    saying AUTH needs TLS, and the connection stays the `default` user with
    whatever it may do; the YAML setting `auth.plaintext: true` lifts it.
29. **Multi-slot merge writes commit atomically by agreement** (2026-11-15):
    MSET, DEL, UNLINK and their kin over several slots go through the atomic
    commit across replica sets, as agreement writes, so ruling 22 holds as
    written. A minority side refuses them; over one slot they stay merge
    writes.
30. **HSCAN, SSCAN and ZSCAN page a large collection** (2026-11-15): one page
    for a collection Valkey keeps compact (at most 128 entries), as Valkey
    answers; a larger one paged by COUNT through a per-element insertion
    sequence, so an element present for the whole scan is returned.
31. **Valkey's persistence and replication commands take Mimir's meaning**
    (2026-11-16): SHUTDOWN stops the node gracefully (Valkey's options
    parsed); SAVE, BGSAVE and BGREWRITEAOF are refused, persistence not
    being available until Dagda (as ruling 17); REPLICAOF, SLAVEOF, SYNC,
    PSYNC, REPLCONF and FAILOVER are refused, Mimir replicating through its
    own cluster; RESTORE-ASKING is RESTORE; MIGRATE is built later, as a
    client of another server, so data can move from Valkey into Mimir.

32. **A plain read serves the last value its node knows committed**
    (2026-11-17): Dannan's agreement never tells a replica that a write
    committed. A node knows a commit from one of three sources: its own
    commit, the commit notice Mimir sends a key's replicas after each commit,
    or a later accept that proves it. A plain read may be stale (ruling 5)
    but never shows a value that is later undone. A connection always reads
    its own writes. A strict read takes a majority round (Dannan's read()).
33. **Whole-keyspace commands are cluster-wide** (2026-11-17): KEYS, SCAN,
    DBSIZE, RANDOMKEY, FLUSHDB and FLUSHALL answer for the whole cluster from
    any node (ruling 1). SCAN's cursor walks slot by slot, each slot asked of
    a replica, and the flushes delete by agreement in every slot.
34. **One path: a lone node is a cluster of one** (2026-11-17): every
    mimir-node is a dannan::Node, and every write goes through agreement,
    even on a genesis set of one node. There is no separate standalone path.
    The claim board's registrar moves to one four-node Mimir cluster when
    stage 3's gate passes.

35. **Agreement only, multi-key writes refused until Dannan 0.10.0**
    (2026-11-17): ruling 34 holds from stage 3's step 3, a lone node
    included, although Dannan 0.9.0 refuses a change of more than one key.
    Until 0.10.0's atomic commit, a write that names more than one key is
    refused on every node, a lone one included. That covers MSET, RENAME,
    LMOVE, SMOVE, the STOREs, multi-key MULTI and scripts, and the flushes.
    The differential cases that use them fail until then, as a known and
    listed gap; no test is removed.

36. **A script writes only the key it declares** (2026-11-17): EVAL,
    EVALSHA and FCALL run by agreement on their one declared key. A write
    inside a script to a key it did not declare is refused, as is any
    command inside it naming another key. A script that declares no key runs
    on its node's committed view and only reads. This is Valkey's
    documented rule ("all keys must be passed explicitly"), not its
    standalone leniency.

37. **A client asks for strict reads with CLIENT STRICT ON|OFF**
    (2026-11-18): a Mimir subcommand of CLIENT, beside Valkey's
    per-connection modes. While it is ON, every read on that connection is
    confirmed with a majority of its key's replicas (Dannan's read()), and is
    refused on a minority side. OFF, the default, gives ruling 32's plain
    reads. CLIENT INFO and CLIENT LIST show it.

38. **No special nodes** (2026-11-20): asked whether a node outside a key's
    replica set should forward, answer MOVED, or look like Valkey's cluster,
    Tom: "There are no 'special' nodes. This is true for every function."
    Every node answers every command for every key itself, as ruling 1 has
    it: a node that holds no replica of a key forwards to one, and the
    client never sees it. No MOVED, no client-visible topology; INFO keeps
    `cluster_enabled:0`.

39. **Scripts and functions are cluster state, by agreement** (2026-11-20):
    FUNCTION LOAD, DELETE, FLUSH and RESTORE commit on one register, as the
    users do (ruling 27). SCRIPT LOAD, and EVAL's caching, write the script
    to a register named by its SHA; an EVALSHA that misses on its node reads
    that register before answering NOSCRIPT. SCRIPT FLUSH clears its node's
    cache only, and SCRIPT EXISTS answers for its node.

40. **Ruling 38 confirmed, and every node's address listed** (2026-11-20):
    no MOVED; the CLUSTER commands answer as standalone Valkey's do. Any
    node also lists every node's client address, so a client whose node
    has gone can find another. It is a Mimir command, not CLUSTER SHARDS,
    which a cluster-mode client would read as slot ownership (the Mimir
    session's choice of where; Tom chose that it is listed).

41. **The shard count: at most 8192, and resizable** (2026-11-20): one
    value for the whole cluster, at most 8192 (Dannan's slot advert is at
    most 1 KiB), changeable on a running cluster. Resizing moves keys
    between shards; it is to be designed, and it needs Dannan's slot table
    to change its count, which the stack's placement plan places.

42. **What Mimir calls itself** (2026-11-20): HELLO answers server "mimir"
    and version "8.1.1", the Valkey behaviour it implements. INFO says
    `redis_version:7.2.4` and `valkey_version:8.1.1` as Valkey 8.1.1 does,
    plus `mimir_version`. Scripts see SERVER_NAME "mimir".

Settled from the group's direction, not ruled (Tom, 2026-11-20: "You
should be able to answer this from the group direction"):
- **Persistence (stage 5): Dagda's durable AcceptorStore in its value-less
  form.** Dagda keeps ballots, summaries and deltas in one fenced,
  group-committed log per space; Mimir keeps the values in its keyspace and
  checkpoints them to Dagda. Durable storage is Dagda's, so Mimir writes
  none of its own (no duplication). Deltas mean a write never costs the
  value's size on disk, and values are not held twice in memory.

## Waiting on Tom

`TODO/ACTIVE.md` keeps the list ("Waiting on Tom" and "Proposed, not yet put
to Tom"), so it is in one place.

## Shape

- **One process per node**, advertising capability `mimir` (stack decision 2).
  Pins a Dannan release tag (0.9.1 on 2026-11-19).
- **Clients** connect over `dannan::listen`'s `StreamListener`; Mimir parses
  RESP2 and RESP3 above it, with a fuzz target. A per-client outbound byte bound
  closes a slow client (Agreed, Dannan and Lir). Under optional client
  certificates, one that did not verify is not an absent one (Lir's lesson).
- **Shards.** Keys hash through Redis's 16384 slots onto S shards, each a
  Dannan agreement slot (ruling 26). A shard is founded on the genesis set
  and held in memory on its replica set, which lives in the slot's own
  acceptors and moves only by Dannan's transfer(), proposed by placement
  (`placement::by_capacity` among nodes advertising `mimir`) as nodes join
  and leave. A node outside the set forwards with `Routes::call` (ruling 38:
  never MOVED).
- **Work classes** (Agreed, Dannan). A client command is foreground and runs on
  the connection's strand; replication a client's write waits on is foreground;
  catch-up is loader; checkpointing, eviction scans and anti-entropy are
  speculative. The worker pool is only for work that blocks or is long.
- **Memory.** `maxmemory` maps onto what Mimir declares and reserves in the
  node's `Budget`; a reserve that cannot be met is the Redis OOM error (Agreed,
  Dannan).

## The two classes of command (Proposed, from rulings 1 to 5)

Every write command has exactly one class, held in the command table beside its
arity and key positions.

**Agreement.** The reply or effect depends on the prior value in a way that
cannot merge: `SET` with `NX`, `XX` or `GET`, `SETNX`, `GETSET`, `GETDEL`,
every pop and move (`LPOP`, `RPOP`, `SPOP`, `ZPOPMIN`, `LMOVE`, and the
blocking forms), `WATCH`/`MULTI`/`EXEC`, scripts and functions, stream ids
generated by the server and consumer groups, and eviction's deletes. The
read-modify-write commands of ruling 3 are in this class by default.

- The node the client is connected to proposes. A command is a function on the
  key's value, committed when a majority of the key's 3 replicas accept it. No
  leader, no lease, no primary. One round when a key is uncontended, two when
  two nodes race for it.
- Several keys commit atomically, in one slot or across slots (rulings 22
  and 29; never `CROSSSLOT`), through Dannan's atomic commit across replica
  sets, which is due in Dannan 0.10.0. Until then a write naming more than
  one key is refused (ruling 35). `WATCH` is a check of each key's write
  count inside the agreed change.
- A blocking pop is a committed pop, so an element is delivered once. Serving
  the longest-waiting client first across nodes needs the waiters recorded in
  the key's replicated state, dropped by incarnation when their node leaves.
- A node that cannot reach a majority of a key's replicas refuses the command
  within its deadline, with an error that says why.

**Merge.** Writes whose effects commute or have a deterministic winner: plain
`SET`, `MSET`, `DEL`, `EXPIRE`, `HSET`, `HDEL`, `SADD`, `SREM`, plain `ZADD`,
pushes, `PFADD`, `SETBIT`.

- Any replica applies the write and sends it to the others, one batched call
  per peer per tick, acknowledged per batch, with per-op sequence numbers so a
  lost batch is caught (Agreed, Dannan). Both sides of a split accept these.
- Each type merges deterministically: registers by hybrid clock then node id;
  observed-remove sets, hashes and sorted sets; a sequence for lists. A TTL is
  an absolute expiry time, itself a register, so replicas with skewed clocks
  agree on what expired (Lir's advice). These types are Mimir's own: Dagda's
  built-in merges order differently and forget old counter slots (Agreed,
  Dagda).
- A dot is (node, incarnation, seq): the incarnation is Dannan's own,
  `Context::identity().durability_epoch()`, fresh on every process start and
  already what membership keys a restart on (Dannan's correction: no id of
  Mimir's own); seq is contiguous per shard within it, so a per-origin version
  vector neither stalls nor skips (the hole Lir named) and no durable counter
  is needed (reviewed by Lir: it closes the hole).
- Folding a departed incarnation's entry (Lir's two conditions). It is folded
  only when the shard's whole replica set on the roster holds the same maximum
  for it, a partitioned replica included, never on the reachable majority
  alone. A restarted node that recovers its previous incarnation's ops from
  its `persisted` log forwards them under their old dots first, and its higher
  maximum holds the fold back until it has. Folded incarnations are kept as a
  small set, and an op from one that arrives later (from a node since removed
  from the roster) is dropped everywhere, deterministically, and counted. An
  op at `ephemeral` that died with its origin is lost, as `ephemeral` says.
- Ruling 3's commands run in this class where configured (per key pattern or
  per connection): counters sum, and two clients may be handed the same `INCR`
  result during a split.

### The seam between the two classes (Proposed)

- **At heal (ruling 4).** Every key carries the version of its last agreed
  commit. A merging write records the agreed version it was made against and
  whether its node could reach a majority at the time. A write made without a
  majority, to a key whose agreed version has since moved, is dropped and
  counted; the count is on Status.
- **In ordinary running.** An agreement command reads the key at a majority. A
  merging write acknowledged at `replicated` is on a majority, so every later
  agreement command sees it. One acknowledged at `ephemeral` or `persisted` may still be on
  one node only: an agreement command that commits before it arrives is ordered
  after it, and the write loses. That is the stated contract (ruling 16).

## Agreement in Dannan (ruling 6; to be designed with the Dannan session)

What Mimir needs, as behaviour:

- A linearizable change to a named register over a replica set the caller
  supplies: any node proposes a function of the current value; it commits on a
  majority; competing proposers are resolved without a leader; several
  registers of one replica set commit atomically in one round.
- Every operation takes a deadline and the caller's executor and returns
  evidence (who accepted, who refused and why), never a boolean.
- Dannan stores nothing: the hosted system holds each acceptor's state behind
  an interface (Mimir in memory, with Dagda behind it; Lugh and Louhi in
  Dagda).
- A node that restarts without its acceptor state is a new incarnation and
  catches up before it votes.
- A strict read confirmed with a majority, and a refusal on a minority side.

Built: it shipped in Dannan 0.9.0 (2026-11-17), with everything below except
several registers committing atomically, which is 0.10.0's. How Mimir uses it
is `TODO/ACTIVE.md`, "What stage 3 rests on"; the reviews are in
`TODO/COMPLETED.md`, "The Dannan 0.9 reviews". The rest of this section is
the history of the request.

Dannan's answer (2026-11-15; Tom confirmed ruling 6 to it directly): built in
Dannan 0.9.0 beside the hybrid clock, its design to be reviewed by Mimir, Lugh
and Dagda before it is built. Dannan's part is the proposer, the wire
messages, ballots, quorum counting and evidence; acceptor state stays behind
an interface the hosted system implements, durably or in memory. Its shaping:
ballots carry (counter, node id, durability epoch), so a restarted node never
reuses one; the acceptor interface is batched per replica set, so a slot's
`MULTI`/`EXEC` is one round; a register's name is opaque bytes scoped by the
hosted system; a strict read is an identity proposal, one mechanism for both.

Recommended starting point: CASPaxos, which is this shape and needs no log.

Dannan's design is `../dannan/TODO/2026-11-15-agreement-design.md` (CASPaxos,
`AcceptorStore`, `Outcome`, a one-round fast path). Mimir's review
(2026-11-15) asked for four things, the second the one that matters most:
- **Large values**: a prepare answers with a ballot and a small summary, not
  the value; the proposer uses its own replica's value when it holds the
  highest ballot; an accept carries a whole value or a delta against a base
  ballot, which the store applies through Mimir's function. Without this an
  LPOP on a 100 MB list moves about 400 MB.
- **Mixing the classes**: the reduction of a majority's prepare answers into
  the current value is Mimir's, defaulting to the highest ballot. Mimir's
  reducer joins in the merge writes some acceptors have seen and others have
  not, so an agreement command sees every merge write acknowledged on a
  majority (ruling 16). Without it the two classes cannot share a key.
- The committed attempt's reply (a popped element, `SET NX`'s answer) returned
  in the `Outcome`, since a change may run more than once across retries.
- Catch-up after a restart owned by the store (a shard's state transferred
  whole), and names as bytes on the wire.

Dannan's simulator found (2026-11-15) that a proposer whose commit's answers
are lost retries and reads its own write. Every agreement command therefore
tags its write with the command's id (node, connection, sequence), and a retry
that finds its own tag answers from the effect already committed, not by
running again: an LPOP retried must not pop twice, and a `SET NX` retried must
answer OK, not nil. (As built, the id is the node, its durability epoch and a
per-node counter.) Dannan's harness (0.9.0) carries Mimir's reducer case as a
named requirement: acceptors at one highest ballot holding different merged
states, with a checker that refuses a reducer that moves the base back.

Lugh's per-user lease for TOTP replay and Louhi's single active scheduler want
the same guarantee and are the second and third consumers; Dagda's leases are
not it (two holders are briefly possible, and fences carry the rest).

## Durability (ruling 7)

- A session sets its `ack_level` with a Mimir command; a command may carry
  one; the YAML key `ack_level` sets the default (ruling 24). Valkey's `WAIT`
  and `WAITAOF` map onto `replicated` and `persisted` where their arguments
  allow (Proposed).
- `decided` for a merging write (Proposed): answered once a majority of the
  key's replicas hold the write in memory, so ruling 16's guarantee holds for
  it as for `replicated`, without waiting on disks. For an agreement command
  it is what `ephemeral` already gives (ruling 23).
- `persisted` and `replicated` are Dagda's log: one op log per shard, group committed.
  Only the node where an op originated appends it, since Dagda refuses an entry
  whose dot's origin is not the appender; one entry carries a tick's contiguous
  run of ops; the log refuses at its bound and never drops the oldest (Agreed,
  Dagda).
- Checkpoint (Agreed with Dagda, for now): snapshot chunks as objects with
  claims and one small head naming every shard's latest, not a tree-backed
  space per shard, which is unmeasured at 10^5 to 10^6 keys and not buildable
  until Dagda's slice 2. The checkpoint stores the version vector and the
  agreed versions atomically with the state; a replica behind the log's
  truncation recovers by checkpoint transfer, never partial replay.
- In Dagda's terms (Agreed, Dagda): each shard's op log is a replicas(3) log
  opened with k = 2; `replicated` is an append at Dagda's replicated,
  `persisted` an append at Dagda's persisted (the appender's own sync, which
  slice 4 must define for logs), and `ephemeral` and `decided` never touch
  Dagda.
- An agreement command always takes its majority round; the level says what
  each acceptor does before it answers (memory, or its disk). At `persisted`
  and `replicated` an acceptor's promise and accepted value go to a node-local log on that
  acceptor, appended under the acceptor's own dot with the proposer's op as
  bytes, never to the shard's replicated op log. A node-local log has no
  rollback, which an acceptor needs: a restored older segment must not make it
  forget a promise (Dagda's recommendation; Tom's ruling when slice 4 is
  built).
- Mimir writes to Dagda as its node principal, so client principals never
  reach the scope check at Dagda's replicas (Agreed, Dagda).
- Unknown until Dagda's slice 4: the log's inline limit, and its rate against a
  Redis write load (accepted at about 8,000 messages a second for Lir).

## Eviction (ruling 9; Proposed)

All of Valkey 8's policies. A victim is chosen by a replica that is over its
ceiling and deleted through agreement, so every node agrees it is gone; a
minority side evicts nothing and refuses writes at its ceiling. Access recency
for LRU and LFU is approximate across nodes: replicas exchange coarse samples
at the speculative class. To be designed in full before it is built.

## Pub/sub (Agreed, Dannan and Lir)

As built (2026-11-17): every publication goes to every node over one notify
channel, `mimir.pubsub`, and each node delivers to its own subscribers. A
notification is raised once, by the committing node; a node's own "expired"
and "keymiss" stay with its own subscribers. Fan-out by announced interest,
below, is a later refinement.

`PUBLISH`, `SUBSCRIBE`, `PSUBSCRIBE` and keyspace notifications ride Dannan's
notify channels directly, at most once, fanned out only to nodes that have
announced interest. Lir's interest index is the reference and is not a library;
when Mimir builds the same, an "interest set by announcement" piece is proposed
for Dannan (rule 3) rather than written twice.

## Authentication (rulings 12 to 14; Agreed, Lugh)

- A pluggable seam giving a per-connection grant, with a built-in user list
  from YAML (Argon2id or random secrets only) and a Lugh plugin.
- Default with Lugh: Mimir-issued random secrets (at least 128 bits) bound to a
  Lugh principal id, stored as HMAC-SHA-256, checked in memory on any node.
  Optional: a Lugh access token as the password, verified in process with
  `lugh_client`.
- ACL rules (command categories, key patterns, channel patterns) are Mimir's
  own replicated state, keyed by Lugh's immutable principal id and optionally
  group id; at most one coarse scope, `mimir:connect`, as a gate.
- Revocation: the plugin subscribes to `lugh/principals/<id>` as an ordinary
  MQTT client of Lir, idempotent on version, and drops or rechecks connections
  authorised under an older version. With no heartbeat within the configured
  bound (default proposed: 30 s) it refuses principals it has not rechecked
  through Lugh's API. Needs Lir's exported MQTT client library (`lir::client`,
  ready on Lir's develop at `542517c`, 2026-11-15; Mimir's needs are inside what Tom approved for it;
  pinned by commit, built with `LIR_BUILD_SERVER=OFF`, until Lir's first tag)
  and Lugh's events, which are designed and not scheduled.
- Mimir's own service principal in Lugh (Agreed, Lugh): an app with the
  client-credentials grant holding `lugh:events.read` (the event topics) and
  the narrow principal-check scope of ruling 18 (rechecks and minting):
  `lugh:principals.status`, read by `GET /api/v1/principals/{id}/status`,
  which answers kind, enabled and version (an ETag) and a 404 when unknown.
  Confirmed by Tom to Lugh; designed in Lugh's
  `TODO/2026-11-15-principal-events.md`, not scheduled, to be built with the
  events. Before minting a secret Mimir
  reads the principal once through `ApiClient` (a user, an app or a group, by
  kind), refuses on a 404 or a disabled principal, and stores the ETag it read
  with the secret: that ETag is the version an event or recheck is compared
  against, and it closes the window between the check and the mint. A recheck
  is the same single read.
- Never: Lugh's credentials stored, cached or proxied; a passwordless default
  user once Lugh is configured; failing open.

## Order (Proposed)

1. The project skeleton, by the style document's checklist.
2. One node: RESP2 and RESP3, the command table with each command's class, and
   every type, tested from Valkey 8's command documentation with a GoogleTest
   differential gate against a Valkey 8 server on the Pis.
3. The cluster by agreement only: every write takes the agreement path, which
   is correct everywhere and majority-only. In progress on Dannan 0.9. In
   memory only: `persisted` and `replicated` are refused (ruling 17).
4. The merge class and the heal rule. Waits on Dannan 0.10.0's peer-witnessed
   high-water mark for the hybrid clock (ruling 8).
5. Persistence, `persisted` and `replicated`, and checkpoints. Waits on
   Dagda's contract v2 (slice 4).
6. Eviction, then the Lugh plugin (waits on Lugh's events and Lir's client
   library).

## What Mimir asks of the others

| Of | What | Status |
|---|---|---|
| Dannan | The agreement primitive (ruling 6) | Shipped in 0.9.0 |
| Dannan | Atomic commit across replica sets (rulings 22, 29) | Planned for 0.10.0 |
| Dannan | The hybrid logical clock (ruling 8) | In 0.9.0; its peer-witnessed high-water mark is 0.10.0's |
| Dannan | "Interest set by announcement" | Not asked yet: pub/sub goes to every node for now |
| Dagda | The log and contract v2 | Slice 4, paused |
| Dagda | A range form of `retire` | Not asked: batched entries keep lists short |
| Lugh | Principal events over Lir | Designed, not scheduled |
| Lir | The exported MQTT client library | Chosen by Tom, not scheduled |
