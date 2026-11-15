# Mimir - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record. The design is
`TODO/2026-11-15-mimir-design.md`: Tom's rulings ("Ruled", 1 to 23), the two
classes of command, agreement in Dannan, durability, and the order.

## Resume here

- On `develop` and pushed: every Valkey type (strings, keys, lists with the
  blocking pops, hashes, sets, sorted sets, bitmaps, HyperLogLog, streams with
  consumer groups), pub/sub, transactions, the server commands clients call,
  and configurable databases (ruling 21). Gates on corvus-uranus; counts in
  `COMPLETED.md`.
- Scripting's EVAL, EVALSHA, their read-only forms and SCRIPT are in, with
  Valkey's libraries (ruling 25) and its read-only sandbox (proxies on
  Debian's Lua; `next`, `pairs`, `ipairs`, `rawget`, `rawset` and
  `setmetatable` see through or refuse them; `#` of a library table is 0).
- FUNCTION and FCALL are in.
- Keyspace notifications are in.
- DUMP and RESTORE are in, FUNCTION DUMP and RESTORE with them.
- SORT and SORT_RO are in.
- Next, in order: the GEO commands, LCS; then
  authentication with the built-in user list (ACL).
- Pinned to Dannan 0.8.3. Dannan's 0.9 (agreement) is on its develop
  (`dc9ede1`); its simulation harness design
  (`../dannan/TODO/2026-11-15-simulation-harness.md`) is Mimir's to review.
- Stage 3 waits on Dannan 0.9.0's agreement primitive and hybrid clock
  (rulings 6, 8, 19); every agreement command tags its write so a retry that
  reads its own effect answers as the first try would (Dannan's simulator
  finding, in the design); ruling 22's cross-slot atomic commit is one piece with
  Kvasir's transaction record, to be proposed jointly when one of us reaches it.
- Known costs to revisit: HSCAN, SSCAN and ZSCAN answer the whole collection in
  one page (Valkey does that for small ones only); a cursor over large ones is
  owed, since a very large page holds the database lock (law 2).
- Hosted CI failed on every job until `ae3c874` (the Dannan URL made a path);
  check the runs from that commit on.
- The differential gate (`Differential.*`, `mimir/test/differential_test.cpp`)
  grows with each type. Run by hand on corvus-uranus: `LC_ALL=C valkey-server --port 7379
  --bind 127.0.0.1 --save "" --appendonly no --daemonize yes`, `mimir-node
  --listen 127.0.0.1 --port 7380 --databases 16 --seconds 60 &`, then `MIMIR_DIFF_MIMIR_PORT=7380
  MIMIR_DIFF_VALKEY_PORT=7379 ./mimir_tests --gtest_filter='Differential.*'`, and
  `valkey-cli -p 7379 shutdown nosave` after. (Valkey 8.1.1 is installed on
  corvus-uranus and corvus-jupiter with its service disabled; either serves.
  `LC_ALL=C` because Valkey's SORT ALPHA collates by its environment's
  locale; Mimir's is byte order, Valkey's under the C locale.)

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends). Sources go by rsync to
  `/root/mimir-work/mimir` with Dannan beside them in `/root/mimir-work/dannan`;
  `chown -R root:root` both after the sync, or git refuses the clone.
- Commit when a step's gate passes; push `develop` to the private remote.

## Waiting on Tom

Nothing blocks the single-node stage (rulings 20 to 24 on 2026-11-15 settled
scripting, databases, cross-slot commands, the levels for agreement and their
names: `ack_level` with `ephemeral`, `persisted`, `decided`, `replicated`, the
group standard relayed by Kvasir).
Asked of him when it is reached:
- `decided` for a merging write (the design's "Durability", Proposed): a
  majority holds it in memory before the answer.
- The shard map: settled by Tom to Dannan (relayed 2026-11-15): "No root
  except at genesis. A root may start with only one node." Names hash to
  fixed slots (Mimir's shards); every slot is touched at founding on the
  genesis set (one node may be all of it); afterwards each slot's set lives in
  its own acceptors and moves only by transfer(), proposed by placement.
  Nodes advertise the slots they serve as a bitmap, a hint; a refusal is the
  authority. Mimir asked Dannan to size the bitmap by the configured shard
  count (ruling 15), not a fixed 32 bytes.

## Proposed, not yet put to Tom

- The shard count is one value for the whole cluster, fixed when the cluster
  is made; a node configured with another is refused at start.
- HELLO says `server: mimir`, `version: 8.1.1` (the Valkey behaviour
  implemented); INFO says `redis_version:7.2.4` and `valkey_version:8.1.1` as
  Valkey 8.1.1 does, plus `mimir_version`. Clients decide features from these.

## Ordered work

1. The skeleton. Done.
2. One node: every Valkey 8 type and command, RESP2 and RESP3, with the
   differential gate. 2a done (strings, keys, connection).
3. The cluster by agreement only. Waits on Dannan 0.9.0's agreement primitive.
4. The merge class and the heal rule. Waits on Dannan 0.9.0's hybrid clock.
5. Persistence, levels 1 and 2, checkpoints. Waits on Dagda's contract v2.
6. Eviction, then the Lugh plugin. The plugin waits on Lugh's principal
   events and status scope; it links `lir::client` (Lir's develop at
   `6b23268`, on Dannan 0.8.3's StreamConnector, fetched from `../lir` with `LIR_BUILD_SERVER=OFF`, until Lir's
   first tag).
