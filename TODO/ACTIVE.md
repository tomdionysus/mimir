# Mimir - Active Work

Last updated: 2026-11-15 (ACL landed). `COMPLETED.md` is the record, with
gate counts. The design is `TODO/2026-11-15-mimir-design.md`: Tom's rulings
("Ruled", 1 to 30), the two classes of command, agreement in Dannan,
durability, and the order.

## Resume here

- State: `develop` at the ACL commit, pushed. Last gate: release 277 passed,
  differential 32 of 32.
- The single-node stage is complete: every Valkey type and command family,
  RESP2 and RESP3, pub/sub, transactions, server commands, configurable
  databases, scripting (EVAL, FUNCTION, Valkey's Lua libraries and read-only
  sandbox), keyspace notifications, DUMP and RESTORE, SORT, GEO, LCS, and ACL
  with AUTH (rulings 12, 27, 28).
- ACL's table (`mimir/command/acl_table.inc`) is generated from Valkey
  8.1.1's COMMAND INFO; the generator and its input are on corvus-uranus in
  `/root/mimir-work/acl/` (`gen.py keyspecs.json`). A new command needs its
  row added the same way. Small gaps owed: acllog-max-len is fixed at 128;
  DRYRUN checks a subcommand against its container's arity.
- Next: stage 3 waits on Dannan 0.9.0 (agreement primitive, simulation
  harness, hybrid clock). Its shape is settled: rulings 6, 8, 19, 22, 23, 26
  (no root but at genesis; Mimir's shards are Dannan's fixed slots, at most
  8192) and 27; agreement commands tag their writes so a retried commit
  answers as the first would.
- Dannan's reconfiguration fix (2026-11-15, Tom approved; Dannan's
  `TODO/2026-11-15-agreement-design.md`, "Reconfiguration: the gap found in
  building it, and the fix"): agreement records are per slot, and a change's
  names share one slot. Dannan's change(), read(), forget() and catch_up()
  take the slot from the caller: Mimir's is CRC16 of the hash tag or key
  mapped onto the shard count, so tagged keys share a shard.
- Atomic commit across slots (Dannan 0.9 plan item 7), which ruling 22's
  cross-slot commands need: Mimir's proposal is
  `TODO/2026-11-15-cross-slot-commit-proposal.md` (a record register in one
  slot, intents in the others), reviewed by Kvasir (intents at the hosted
  system's granularity, its own conflict rule, an HLC commit stamp, known
  writes) and noted in Dannan's plan under item 7, after the fast path, the
  harness, the file-system interface and the hybrid clock. Ruling 29: MSET
  and DEL over several slots commit through it.
- Dannan's reconfiguration is built on its `feature/agreement-reconfiguration`
  (`c524a28`, not merged): per-slot records, transfer(), settle, a
  caller-named slot, a slot-table fingerprint per request, hint(space, slot).
  The AcceptorStore interface changed (load(slot, names, every, done),
  keep(slot, held, done), optional records()): read `agree/agreement.h` there
  before building Mimir's store.
- Pinned to Dannan 0.8.3. Dannan's 0.9 is on its develop.
- In hand: ruling 30, HSCAN, SSCAN and ZSCAN paging a collection of more
  than 128 entries through a per-element insertion sequence.

## The differential gate

`Differential.*` in `mimir/test/differential_test.cpp`, run by hand on
corvus-uranus or corvus-jupiter (Valkey 8.1.1 on both, service disabled):

```
LC_ALL=C valkey-server --port 7379 --bind 127.0.0.1 --save "" --appendonly no --daemonize yes
./mimir-node --listen 127.0.0.1 --port 7380 --databases 16 --auth-plaintext --seconds 200 &
MIMIR_DIFF_MIMIR_PORT=7380 MIMIR_DIFF_VALKEY_PORT=7379 ./mimir_tests --gtest_filter='Differential.*'
valkey-cli -p 7379 shutdown nosave; pkill mimir-node
```

`LC_ALL=C` because Valkey's SORT ALPHA collates by its environment's locale.
Valkey's own answers are the expected values: probe it first, write the case,
then the code.

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends; a note before any install). Sources
  go by rsync (`--no-o --no-g`, excluding `build*`) to `/root/mimir-work/mimir`
  with Dannan beside them; `chown -R root:root` after the sync. Build:
  `MIMIR_CI_JOBS=2 nice -n 19 tools/ci.sh release`, or an incremental
  `cmake --build build-ci/release -j2`.
- The Lua packages (liblua5.1-0-dev, lua-cjson-dev, lua-bitop-dev,
  lua-cmsgpack) are on uranus and jupiter; clang and libFuzzer on jupiter.
  Fuzz targets on the Pis: `-DMIMIR_FUZZ_SANITIZERS=fuzzer,undefined` (ASan
  cannot start there).
- Commit when a step's gate passes; push `develop`. Read the completed hosted
  CI runs, not only the newest (superseded runs are cancelled); push no more
  until a run completes when a fix awaits confirmation.

## Waiting on Tom

- Hosted CI starts no job (since 2026-11-15 22:25Z): GitHub answers "recent
  account payments have failed or your spending limit needs to be
  increased". Every run since `d0766f5` is red for that alone; the Pi gates
  stand meanwhile. Read the first run after billing is fixed.

- The shard count's cap of 8192 (Dannan's advert is at most 1 KiB):
  recommended to accept as the upper bound of "configurable" (ruling 15).
- `decided` for a merging write (the design's "Durability", Proposed): a
  majority holds it in memory before the answer. Asked when stage 3 is reached.

## Proposed, not yet put to Tom

- The shard count is one value for the whole cluster, fixed when the cluster
  is made; a node configured with another is refused at start (Dannan now
  carries it in the cluster's identity).
- HELLO says `server: mimir`, `version: 8.1.1`; INFO says
  `redis_version:7.2.4` and `valkey_version:8.1.1` as Valkey 8.1.1 does, plus
  `mimir_version`; scripts see `SERVER_NAME` "mimir".

## Ordered work

1. The skeleton. Done.
2. One node: every Valkey 8 command, RESP2 and RESP3, with the differential
   gate. Done.
3. The cluster by agreement only. Waits on Dannan 0.9.0.
4. The merge class and the heal rule. Waits on Dannan 0.9.0's hybrid clock.
5. Persistence, `persisted` and `replicated`, checkpoints. Waits on Dagda's
   contract v2.
6. Eviction, then the Lugh plugin. The plugin waits on Lugh's principal
   events and status scope; it links `lir::client` (Lir's develop at
   `6b23268`, on Dannan 0.8.3, until Lir's first tag).
