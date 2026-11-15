# Mimir - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record. The design is
`TODO/2026-11-15-mimir-design.md`: Tom's rulings ("Ruled", 1 to 18), the two
classes of command, agreement in Dannan, durability, and the order.

## Resume here

- Steps 1, 2a and 2b's first part are on `develop` and pushed: strings, keys,
  connection commands, lists (not blocking), hashes, sets, sorted sets. Gates
  on corvus-uranus; counts in `COMPLETED.md`.
- Since: blocking pops, bitmaps, HyperLogLog, pub/sub, transactions, and the
  stream log (XADD, XRANGE, XREVRANGE, XLEN, XDEL, XTRIM, XREAD with BLOCK,
  XSETID).
- Next, in order: stream consumer groups (XGROUP, XREADGROUP, XACK,
  XPENDING, XCLAIM, XAUTOCLAIM, XINFO); the server commands clients call
  (CONFIG GET/SET, CLIENT LIST/KILL/INFO, INFO's sections, OBJECT, DUMP and
  RESTORE, keyspace notifications); then scripting (needs Tom: Lua is a
  dependency).
- Known costs to revisit: HSCAN, SSCAN and ZSCAN answer the whole collection in
  one page (Valkey does that for small ones only); a cursor over large ones is
  owed, since a very large page holds the database lock (law 2).
- Hosted CI failed on every job until `ae3c874` (the Dannan URL made a path);
  check the runs from that commit on.
- The differential gate (`Differential.*`, `mimir/test/differential_test.cpp`)
  grows with each type. Run by hand on corvus-uranus: `valkey-server --port 7379
  --bind 127.0.0.1 --save "" --appendonly no --daemonize yes`, `mimir-node
  --listen 127.0.0.1 --port 7380 --seconds 60 &`, then `MIMIR_DIFF_MIMIR_PORT=7380
  MIMIR_DIFF_VALKEY_PORT=7379 ./mimir_tests --gtest_filter='Differential.*'`, and
  `valkey-cli -p 7379 shutdown nosave` after. (Valkey 8.1.1 is installed there
  with its service disabled.)

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends). Sources go by rsync to
  `/root/mimir-work/mimir` with Dannan beside them in `/root/mimir-work/dannan`;
  `chown -R root:root` both after the sync, or git refuses the clone.
- Commit when a step's gate passes; push `develop` to the private remote.

## Waiting on Tom

Nothing blocks the single-node stage (rulings 20 to 23 on 2026-11-15 settled
scripting, databases, cross-slot commands and the levels for agreement).
Asked of him when it is reached:
- The switch's spelling, one for Mimir and Kvasir. Tom ruled for
  Kvasir (2026-11-15, Kvasir's design, "Acknowledgement levels"): level 1 is
  local disk with the cluster's decision after, the default level is YAML with
  no built-in value (a node without one refuses to start), and on a minority
  side 0 and 1 are held and decided at heal while 2 is refused. Mimir's design
  proposed that an agreement command treats 1 as 2; Kvasir's ruling went the
  other way. Mimir asks Tom whether its levels follow Kvasir's rulings, and
  for one spelling of the switch for both projects (Kvasir proposes a session
  variable, a per-statement hint and a YAML key).
- Scripting (`EVAL`, `FUNCTION`): Valkey's is Lua 5.1, which would be a
  dependency (Lua's own C sources, MIT). Recommendation to come with the step.
- Databases: Mimir has one (SELECT 0 only; COPY ... DB n refused), as
  Valkey's cluster mode; standalone Valkey has sixteen. Found by the
  differential gate.
- Cross-slot multi-key commands (`MGET`, `DEL` and `MSET` over keys in
  different slots): Valkey's cluster mode refuses them (`CROSSSLOT`); its
  standalone mode allows them. Mimir is one server to its clients, so the
  question is whether to accept them, atomically or per key.

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
   `542517c`, fetched from `../lir` with `LIR_BUILD_SERVER=OFF`, until Lir's
   first tag).
