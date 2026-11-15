# Mimir - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record. The design is
`TODO/2026-11-15-mimir-design.md`: The maintainer's rulings ("Ruled", 1 to 18), the two
classes of command, agreement in Dannan, durability, and the order.

## Resume here

- Step 1 (the skeleton) and step 2a (RESP, the keyspace, strings, keys,
  connection commands, `mimir-node`) are on `develop`. Gate: `tools/ci.sh
  release` on node-d; the count is in `COMPLETED.md`.
- Next: step 2b, the remaining types, in this order: lists, hashes, sets,
  sorted sets, then bitmaps and HyperLogLog, streams, pub/sub, transactions
  (`MULTI`/`EXEC`/`WATCH`), then scripting (needs the maintainer: Lua is a dependency).
- The differential gate (`Differential.*`, `mimir/test/differential_test.cpp`)
  grows with each type. Run by hand on node-d: `valkey-server --port 7379
  --bind 127.0.0.1 --save "" --appendonly no --daemonize yes`, `mimir-node
  --listen 127.0.0.1 --port 7380 --seconds 60 &`, then `MIMIR_DIFF_MIMIR_PORT=7380
  MIMIR_DIFF_VALKEY_PORT=7379 ./mimir_tests --gtest_filter='Differential.*'`, and
  `valkey-cli -p 7379 shutdown nosave` after. (Valkey 8.1.1 is installed there
  with its service disabled.)

## How work is done

- No builds or test runs on the Mac. Every build is on the build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends). Sources go by rsync to
  `/root/mimir-work/mimir` with Dannan beside them in `/root/mimir-work/dannan`;
  `chown -R root:root` both after the sync, or git refuses the clone.
- Commit when a step's gate passes; push `develop` to the private remote.

## Waiting on the maintainer

Nothing blocks the single-node stage. Asked of him when it is reached:
- Scripting (`EVAL`, `FUNCTION`): Valkey's is Lua 5.1, which would be a
  dependency (Lua's own C sources, MIT). Recommendation to come with the step.
- Databases: Mimir has one (SELECT 0 only; COPY ... DB n refused), as
  Valkey's cluster mode; standalone Valkey has sixteen. Found by the
  differential gate.
- Cross-slot multi-key commands (`MGET`, `DEL` and `MSET` over keys in
  different slots): Valkey's cluster mode refuses them (`CROSSSLOT`); its
  standalone mode allows them. Mimir is one server to its clients, so the
  question is whether to accept them, atomically or per key.

## Proposed, not yet put to the maintainer

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
