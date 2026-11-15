# Mimir - Active Work

Last updated: 2026-11-15 (end of session). `COMPLETED.md` is the record, with
gate counts. The design is `TODO/2026-11-15-mimir-design.md`: Tom's rulings
("Ruled", 1 to 28), the two classes of command, agreement in Dannan,
durability, and the order.

## Resume here

- State: `develop` at `d0766f5`, pushed; hosted CI green on all seven jobs
  (last checked at `d4f18e9`). Last gate: release 257 passed, differential 31
  of 31.
- The single-node stage is complete but for authentication: every Valkey
  type and command family, RESP2 and RESP3, pub/sub, transactions, server
  commands, configurable databases, scripting (EVAL, FUNCTION, Valkey's Lua
  libraries and read-only sandbox), keyspace notifications, DUMP and RESTORE,
  SORT, GEO, LCS.
- Next: authentication, Valkey's ACL, under rulings 12, 27 and 28. Nothing of
  it is in code yet. Prepared:
  - The command-to-category table, taken from Valkey 8.1.1's own `ACL CAT`
    answers for Mimir's commands and ACL's subcommands (310 entries, Valkey's
    category order): `build/acl-categories.inc` (scratch, not committed; the
    source data and the generating script's inputs are on corvus-uranus in
    `/root/mimir-work/acl/`: `cats.txt`, `members.txt`, `mimir.txt`). A new
    command needs its categories added the same way.
  - Valkey's client-visible ACL behaviour was probed (GETUSER, LIST, SETUSER
    rule errors, DRYRUN, AUTH, NOPERM errors, LOG, CAT, GENPASS, SAVE/LOAD
    refusal). Re-probe on uranus while writing; the differential is the gate.
  - The shape agreed with the rulings: users in memory, seeded from YAML
    (lines in ACL-file form) at start, SETUSER/DELUSER changes held by the
    database (in the cluster, by agreement, ruling 27); passwords as SHA-256
    hex; Session gains its user, whether it is authenticated, and whether it
    is on TLS; `auth.plaintext` (default false) gates AUTH and HELLO AUTH on
    plaintext connections (ruling 28; the differential runs mimir-node with
    it on). ACL checks in Database::execute after arity, before queueing in
    MULTI, and in a script's redis.call: command (by "cmd|sub" for
    containers), then keys (R for readonly, W for write commands), then
    channels. NOAUTH when the default user is off or has a password and the
    session has not authenticated.
  - Description strings follow Valkey: "+@all "/"-@all " then the kept rule
    string, a later rule replacing an earlier one for the same command (and
    its subcommands); keys "~p", "%R~p", "%W~p"; channels with
    "resetchannels" in ACL LIST unless allchannels.
- After authentication: stage 3 waits on Dannan 0.9.0 (agreement primitive,
  simulation harness, hybrid clock). Its shape is settled: rulings 6, 8, 19,
  22, 23, 26 (no root but at genesis; Mimir's shards are Dannan's fixed
  slots, at most 8192) and 27; agreement commands tag their writes so a
  retried commit answers as the first would.
- Pinned to Dannan 0.8.3. Dannan's 0.9 is on its develop.
- Known costs to revisit: HSCAN, SSCAN and ZSCAN answer the whole collection
  in one page; a cursor over large ones is owed (law 2).

## The differential gate

`Differential.*` in `mimir/test/differential_test.cpp`, run by hand on
corvus-uranus or corvus-jupiter (Valkey 8.1.1 on both, service disabled):

```
LC_ALL=C valkey-server --port 7379 --bind 127.0.0.1 --save "" --appendonly no --daemonize yes
./mimir-node --listen 127.0.0.1 --port 7380 --databases 16 --seconds 200 &
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
   gate. Done but for authentication (ACL), next.
3. The cluster by agreement only. Waits on Dannan 0.9.0.
4. The merge class and the heal rule. Waits on Dannan 0.9.0's hybrid clock.
5. Persistence, `persisted` and `replicated`, checkpoints. Waits on Dagda's
   contract v2.
6. Eviction, then the Lugh plugin. The plugin waits on Lugh's principal
   events and status scope; it links `lir::client` (Lir's develop at
   `6b23268`, on Dannan 0.8.3, until Lir's first tag).
