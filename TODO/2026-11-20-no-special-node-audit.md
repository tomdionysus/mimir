# Mimir: audit for special roles and nodes (2026-11-20)

Tom, to the stack: "There is to be no concept of a special role or node. Any
node may disappear at any time, have patchy connectivity, may be permanently
retired, be slow, or be degraded in some other way. The purpose of every one
of your projects is so that is never the user's problem except in
exceptional circumstances." Reported to Lir Development Cluster for the
stack's placement plan. Nothing changes until the plan converges.

## 1. Special roles Mimir has or assumes

- **The genesis set is every shard's replica set.**
  - Every change, read, commit notice and catch-up uses the agreement
    table's genesis nodes at version 0 (`mimir/cluster/agreements.cpp`:
    change, read, notify, recover).
  - Nodes that join later hold no replica of anything. Retire a majority
    of the genesis set and every write is refused for good.
  - This is the one real special set. Stage 3 step 4 (placement by
    transfer()) was always meant to end it.
- **Bootstrap peers** (`cluster.bootstrap`). A list of addresses used only
  to join; any one live peer suffices. This is Dannan's, and it is not a
  role.
- **Founding.** A node with no genesis list and no bootstrap peers founds
  a cluster of one (ruling 26: a root only at genesis). It plays no part
  afterwards.
- **None of these exist:** a leader, primary, coordinator, lowest-id rule
  or fixed address. Every node proposes for every key.
  - Expiry deletions are proposed by every node that finds the key
    expired; there is deliberately no designated expirer.
  - Scripts and ACL are found from any node.
- **The registrar deployment** is four independent nodes until stage 3's
  gate. Each Pi writes to all four.

## 2. Failure modes today

- **A node gone suddenly.**
  - Writes go on while a majority of the key's replicas answer. Its
    clients lose their connection: a client with one address is stuck
    until it is pointed elsewhere.
  - A command whose outcome the client never saw may have committed. That
    is the same as Valkey, and a client's retry of INCR can apply twice:
    operation ids are per proposal, not per client request.
  - A restarted node catches up before it votes. If a majority of a
    shard's replicas lose their memory at once, that shard's data is lost
    until stage 5 (persistence). The user sees it.
- **Patchy connectivity.**
  - The minority side answers writes with CLUSTERDOWN (agreement only
    until stage 4's merge class) and serves plain reads, possibly stale
    (ruling 5). Strict reads are refused there.
  - A lost commit notice leaves that node's view of the key stale until
    its next write, a strict read, or a restart. There is no
    anti-entropy, so a key never written again stays stale on that node.
- **A node permanently retired.** Its shards keep the genesis set, short
  one member. A second genesis retirement leaves every shard without a
  majority, and every write is refused for good. This is the worst gap.
- **A slow node.**
  - As a replica, it is not waited for when the others answer.
  - As a proposer, only its own clients are slowed.
  - Every node proposing expiries adds rounds (one is applied; the rest
    change nothing).
- **A degraded node.**
  - **Clock off.** A write's speculation and an expiry's deletion decide
    expiry at the proposer's issue time. One node whose clock runs ahead
    can expire keys early for the whole cluster. This is a real hole.
  - **Memory full.** Everything is in memory, maxmemory and eviction are
    not built, and the node can fail.
  - **Half-broken.** A node that takes connections but cannot reach its
    peers answers CLUSTERDOWN to its clients, who are not moved
    elsewhere.

## 3. What Mimir relies on from below

From Dannan:
- membership and failure detection;
- agreement at a majority;
- transfer() and settle, to move a slot's replicas;
- catch_up();
- joining through any peer;
- the hybrid clock.

Not yet given, or not yet used:
- **Keeping each slot's replica set whole as nodes join and retire,
  without a leader.** Dannan has transfer(), but which transfer to propose,
  and when, is left to each hosted system.
- **Bounded clock skew.** The 0.10.0 peer-witnessed high-water mark covers
  issuing stamps, not a proposer's wall clock used for expiry.
- **Change notices with gap reporting.**

From Dagda: the durable AcceptorStore (stage 5), so that a majority
restart is not data loss.

## 4. What should change, and where

1. **Replica sets kept whole automatically: in Dannan.** Every agreement
   user (Mimir, Louhi, Kvasir) needs a slot's replica set to follow
   membership: members join, a retired or long-dead member is replaced,
   by transfer(), decided without a leader (each node computing the same
   placement, and the transfer itself an agreement). Mimir's step 4 should
   use that rather than write its own.
2. **Commit notices on a Dannan channel with gaps reported, or
   anti-entropy per slot: in Dannan.** The agreement knows the ballot and
   the replica set. This is the same piece as audit item b.
3. **Time for expiry: Mimir, on Dannan's clock.** An expiry or a write's
   speculation should decide on the hybrid clock, not a node's raw wall
   clock, and acceptors should refuse an issue time beyond their own clock
   by more than a bound. The bound and the check are Dannan's, beside the
   clock; Mimir only uses them.
4. **Clients of a lost node: mostly the client's.** Mimir can make it easy
   by having every node advertise the others' client addresses (CLUSTER
   SHARDS or HELLO are Valkey's places; ruling 38 forbids MOVED, but not a
   list of nodes) and by using DNS. Until then it is the exceptional case.
5. **Data loss on a majority restart: Dagda's durable store** (stage 5).
6. **Retry of a command whose answer was lost: Mimir.** A client id plus
   a request sequence, as Valkey's clients do not send. This is the
   exceptional case and stays as Valkey's.
