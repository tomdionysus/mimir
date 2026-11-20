# Mimir - Active Work

Last updated: 2026-11-19. `COMPLETED.md` is the record, with gate counts. The
design is `TODO/2026-11-15-mimir-design.md`: Tom's rulings (1 to 39), the two
classes of command, agreement in Dannan, durability, and the order.

## Resume here

Next: the stack-wide placement plan Tom asked for (2026-11-20), negotiated
through Lir Development Cluster
(`../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`);
Mimir's audit is `TODO/2026-11-20-duplication-audit.md`. Pieces of Mimir
may move to Dannan; nothing moves until Tom decides. Meanwhile, stage 3:
cluster-wide keyspace commands (3.6); routing (3.3) needs placement (3.4).

State on 2026-11-20:

- `develop` is pushed. Hosted CI read green at `3aee353`.
- Last gate: release 361 passed (corvus-neptune, 2026-11-20). The per-case
  differential (`tools/differential.sh build-ci/release`) shows 233
  differing replies, all accounted for: ruling 35 refusals (157 replies
  carry its words), 4 undeclared-key script writes (ruling 36), and the
  rest cases whose setup used a refused command.
- Dannan is pinned at 0.9.2 (2026-11-20), the stack's version. On a Pi,
  clone `build/dannan-full-0.9.1.bundle`, then fetch
  `build/dannan-0.9.2.bundle`'s tag.
- The Pis were wiped to bare OS on 2026-11-18. The work tree is back on
  corvus-neptune only (`/root/mimir-work`, with Dannan, the Lua dev
  packages and Valkey 8.1.1). Another Pi needs the same: see "How work is
  done".

## The differential gate

`Differential.*` in `mimir/test/differential_test.cpp`, run by hand on a Pi
against Valkey 8.1.1:

```
LC_ALL=C valkey-server --port 7379 --bind 127.0.0.1 --save "" --appendonly no --daemonize yes
./mimir-node --listen 127.0.0.1 --port 7380 --databases 16 --auth-plaintext --seconds 200 & node=$!
MIMIR_DIFF_MIMIR_PORT=7380 MIMIR_DIFF_VALKEY_PORT=7379 ./mimir_tests --gtest_filter='Differential.*'
valkey-cli -p 7379 shutdown nosave; kill $node   # never pkill: the Pis run mimir.service
```

- `LC_ALL=C` because Valkey's SORT ALPHA collates by its environment's
  locale.
- Valkey's own answers are the expected values: probe it first, write the
  case, then the code.
- Since stage 3, FLUSHALL is refused (ruling 35), so the cases cannot share
  one node. `tools/differential.sh [build dir]` runs each case against a
  fresh mimir-node, stops each by its PID, and counts the differing replies
  by cause; its log is `<build dir>/differential.log`.
- Valkey 8.1.1 (Debian's valkey-server, service disabled) is on
  corvus-neptune since the wipe. Post a note on the board before installing
  it elsewhere.

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim: `claims_list`, `claim_take` with the cores the
  `-j` needs, `nice 19`, `claim_release` when it ends, a note before any
  install. When no cores are free, `claim-board wait` in the background;
  never poll.
- After the 2026-11-18 wipe the Pis have build-essential, cmake, ninja, git,
  gdb, clang and lld 19 with the sanitizer runtimes, perf, and the boost,
  ssl, yaml-cpp, gtest and systemd dev packages (Lir Development Cluster).
  The Lua runtime packages are there for mimir.service, but not the dev
  ones: Mimir's build needs `liblua5.1-0-dev`, `lua-cjson-dev` and
  `lua-bitop-dev` (installed on neptune 2026-11-20; elsewhere, a note on
  the board first). A Dannan clone from the bundle warns that the remote
  HEAD is missing; FetchContent checks out the tag and does not mind.
- Putting the work tree back on a Pi:
  - sources by rsync (`--no-o --no-g`, excluding `build*`) to
    `/root/mimir-work/mimir`, then `chown -R root:root`;
  - Dannan beside it at `/root/mimir-work/dannan`. A Pi has no GitHub
    credentials, so it takes Dannan by bundle:
    `build/dannan-full-0.9.1.bundle` here is the whole repository at 0.9.1
    (clone from it). A later tag goes as
    `git bundle create <file> refs/tags/<tag> ^<previous>` in `../dannan`,
    then `git fetch <file> "refs/tags/<tag>:refs/tags/<tag>"` on the Pi.
  - build: `MIMIR_CI_JOBS=2 nice -n 19 tools/ci.sh release`, or an
    incremental `cmake --build build-ci/release -j2`.
- corvus-uranus has less memory than the others (3 to 4 GB): `-j1` for heavy
  translation units, and no sanitizer builds there.
- Sanitizer runs (ASan, UBSan, TSan) only before a release, or to hunt a
  named bug that cannot be pinned any other way; never as a step's gate
  (Tom, restated 2026-11-17). Name the release or the bug before starting
  one. Mimir's gate is release plus differential. Hosted CI has four linux
  jobs (check, release on x86-64 and arm64, clang) and coverage; its clang
  job catches thread-safety annotations the Pis' GCC gate cannot.
- Fuzz targets build with clang 19 and the default
  `fuzzer,address,undefined`.
- Commit when a step's gate passes; push `develop`. Read the completed
  hosted CI runs, not only the newest (superseded runs are cancelled); push
  no more until a run completes when a fix awaits confirmation.
- A test node is stopped by its own PID, never `pkill -x mimir-node`: every
  Pi runs mimir.service.
- ACL's table (`mimir/command/acl_table.inc`) is generated from Valkey
  8.1.1's COMMAND INFO by `build/acl-gen.py` (kept here; `build/` is not in
  git). A new command's row: run mimir-node, `COMMAND LIST` into mimir.txt,
  Valkey's `-3 --json COMMAND INFO` of those into keyspecs.json, then
  `acl-gen.py keyspecs.json`.

## Running: the claim board's registrar

- mimir.service runs on all four Pis on port 6390, four independent nodes,
  from develop `79b62bb` (2026-11-16, before stage 3). Users: `default off`,
  `registrar` (writes `node:*`), `reader` (reads). Plaintext AUTH on the lab
  LAN (ruling 28's `auth.plaintext`, this deployment only). Passwords:
  `/Users/tom/.claude/mimir-registrar.yaml`, never in the repository. Spec:
  `../lir-develop-cluster/TODO/2026-11-16-claim-board-registrar.md`.
- What it needs on a Pi, all kept through the 2026-11-18 wipe and checked
  after it:
  - `/opt/mimir` (bin/mimir-node, lib/libmimir.so.0.1,
    lib/libdannan.so.0.8), found through the unit's
    `LD_LIBRARY_PATH=/opt/mimir/lib`;
  - `/etc/mimir/config.yaml` and `secrets.yaml`, and the unit
    (`tools/deploy/`), which is `Restart=always`;
  - the packages libssl3, libyaml-cpp0.8, zlib1g, libzstd1, liblua5.1-0,
    lua-cjson, lua-cmsgpack and lua-bitop.
  It keeps no state directory.
- To upgrade: build on a Pi, package bin/mimir-node and lib/ (libmimir,
  libdannan), unpack to `/opt/mimir`, `systemctl restart mimir`. Entries
  come back with the registrar's next refresh.
- It moves to one four-node cluster when stage 3's gate passes (ruling 34).
  That needs Dannan 0.10.0, since the registrar's writes must not be refused
  by ruling 35. Do not upgrade it to a stage 3 build before then without
  checking every command the claim board sends.

## Waiting on Tom

- The shard count's cap of 8192 (Dannan's advert is at most 1 KiB):
  recommended to accept as the upper bound of "configurable" (ruling 15).
- `decided` for a merging write (the design's "Durability", Proposed): a
  majority holds it in memory before the answer. To be asked when stage 4
  starts.

## Proposed, not yet put to Tom

- The shard count is one value for the whole cluster, fixed when the cluster
  is made; a node configured with another is refused at start (Dannan
  carries it in the cluster's identity).
- HELLO says `server: mimir`, `version: 8.1.1`; INFO says
  `redis_version:7.2.4` and `valkey_version:8.1.1` as Valkey 8.1.1 does, plus
  `mimir_version`; scripts see `SERVER_NAME` "mimir".

## What stage 3 rests on

Settled with Dannan in review (the record is in `COMPLETED.md`, "The Dannan
0.9 reviews"); what the code relies on:

- Agreement is per slot. Mimir's shard is Valkey's slot (CRC16 of the hash
  tag or key) mapped onto the shard count in contiguous ranges, so tagged
  keys share a shard. A change names one key: Dannan refuses more until
  0.10.0's atomic commit.
- A key's register is named by its database and key. Its value is Mimir's
  record: the DUMP payload, the absolute expiry, the recent operation ids
  with their replies, and a serial (the count of committed writes).
- Every change is repeatable. It carries an operation id (node, durability
  epoch, counter); a record already holding the id answers the recorded
  reply and changes nothing, so an unknown outcome is resolved by proposing
  the same change again.
- The store is in memory, keeps no ancestry, answers `need_whole` to a
  delta, and is not ready after a restart until caught up.
- A write is run speculatively on a scratch keyspace holding only its key,
  with a fixed clock and a seeded random source and every side effect held
  back; on commit the record is installed into the live keyspace and the
  side effects run once.
- Each node's live keyspace is its committed view (ruling 32). A commit
  reaches the other replicas by a best-effort commit notice (user type 600),
  installed only if its version, the committing ballot, is newer.
- The replicas are the genesis set at version 0 until placement moves slots
  (step 6).

## Ordered work

1. The skeleton. Done.
2. One node: every Valkey 8 command, RESP2 and RESP3, with the differential
   gate. Done. Small things still owed, none waiting on anyone: MEMORY
   STATS; PFDEBUG; acllog-max-len is fixed at 128; ACL DRYRUN checks a
   subcommand against its container's arity.
3. The cluster by agreement only (rulings 32 to 37). Done so far, each in
   `COMPLETED.md`: the register and the acceptor store; the node as a
   dannan::Node, a lone one a cluster of one; single-key writes by
   agreement; commit notices; transactions and WATCH over one key; blocking
   pops on one key; scripts on their one declared key; pub/sub and keyspace
   notifications across the cluster; catch-up after a restart; strict reads
   (ruling 37); expiry by agreement; ACL users by agreement (ruling 27);
   scripts and function libraries by agreement (ruling 39).
   Still to do, in this order:
   3. **Routing** (ruling 38, no special nodes): a node outside a key's
      set forwards the command to a replica (Routes::call), and the client
      never sees it; never MOVED, and the CLUSTER commands answer as
      standalone Valkey's do. With it: read-your-writes when a write was
      forwarded. Only arises once placement moves slots (step 4), so it is
      built with it.
   4. **Membership.** Placement moves slots by transfer() as nodes join and
      leave; the replicas then come from the slot's record, not the genesis
      set.
   5. **change_deltas** on a local base: deltas from the live value, with
      the summaries, so an agreement write to a large collection is not
      O(n) at the proposer.
   6. **Cluster-wide keyspace commands** (ruling 33): KEYS, SCAN, DBSIZE,
      RANDOMKEY slot by slot. The flushes wait for 0.10.0 (ruling 35).
   9. Smaller gaps: a script that only reads but declares its key counts as
      a write for WATCH; per-channel interest for pub/sub, where every
      publication goes to every node today; ACL SETUSER and DELUSER inside
      MULTI are refused in a cluster.
   10. **Gates**: the differential against a cluster of one and a cluster
       of three; Dannan's SimNodes run with Mimir's actors under faults,
       checked for linearizability (the checkers link as
       `dannan::sim_checkers`; SimNodes needs `DANNAN_TEST_TEMP_ROOT`
       defined by Mimir's tests); then the registrar moved to one four-node
       cluster.

   Waiting on Dannan 0.10.0, its atomic commit across sets: every write
   naming more than one key (MSET, RENAME, LMOVE, SMOVE, the STOREs,
   multi-key MULTI, scripts and blocking pops, the flushes). Mimir's
   proposal for it is `TODO/2026-11-15-cross-slot-commit-proposal.md`.
4. The merge class and the heal rule. Waits on Dannan 0.10.0's
   peer-witnessed high-water mark (Tom, 2026-11-17): without key custody the
   hybrid clock's ceiling is a plain durable file, so a restored state
   directory could issue stamps again; with it, a node issues no stamp until
   its clock passes the highest stamp a majority of peers report from it.
5. Persistence, `persisted` and `replicated`, checkpoints. Waits on Dagda's
   contract v2.
6. Eviction, then the Lugh plugin. The plugin waits on Lugh's principal
   events and status scope; it links `lir::client`.
