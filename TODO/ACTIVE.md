# Mimir - Active Work

Last updated: 2026-11-16 (ruling 31, the registrar). `COMPLETED.md` is the record, with
gate counts. The design is `TODO/2026-11-15-mimir-design.md`: The maintainer's rulings
("Ruled", 1 to 31), the two classes of command, agreement in Dannan,
durability, and the order.

## Resume here

- State: `develop` pushed (the remote is public since 2026-11-15). Last gate:
  release 310 passed, differential 33 of 33 (node-a, whose build tree
  is `/root/mimir-work/mimir/build-ci`; node-d is back but 4 GB). Hosted CI
  green on all seven jobs at `eb17618` (it runs again since the repository
  went public; its clang and ASan jobs catch what the build servers' GCC gate cannot:
  read every completed run).
- Single-node work still owed (none waits on anyone): MEMORY STATS; PFDEBUG.
- The single-node stage is complete: every Valkey type and command family,
  RESP2 and RESP3, pub/sub, transactions, server commands, configurable
  databases, scripting (EVAL, FUNCTION, Valkey's Lua libraries and read-only
  sandbox), keyspace notifications, DUMP and RESTORE, SORT, GEO, LCS, and ACL
  with AUTH (rulings 12, 27, 28).
- ACL's table (`mimir/command/acl_table.inc`) is generated from Valkey
  8.1.1's COMMAND INFO; the generator is `build/acl-gen.py` here and in
  `/root/mimir-work/acl/` on node-a (and node-d). A new command's row: run
  mimir-node, `COMMAND LIST` into mimir.txt, Valkey's `-3 --json COMMAND
  INFO` of those into keyspecs.json, then `acl-gen.py keyspecs.json`. Small gaps owed: acllog-max-len is fixed at 128;
  DRYRUN checks a subcommand against its container's arity.
- Next: stage 3 waits on Dannan 0.9.0 (agreement primitive, simulation
  harness, hybrid clock). Its shape is settled: rulings 6, 8, 19, 22, 23, 26
  (no root but at genesis; Mimir's shards are Dannan's fixed slots, at most
  8192) and 27; agreement commands tag their writes so a retried commit
  answers as the first would.
- Dannan's reconfiguration fix (2026-11-15, the maintainer approved; Dannan's
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
- Dannan's estimates (2026-11-16, in sessions): change(), read(), forget()
  with the caller-named slot, transfer(), settle and the fast path merge to
  Dannan's develop this session; value-less prepare and deltas change
  AcceptorStore once more, about 1 session, after which the interface should
  hold; the hybrid clock 4 to 6; cross-slot atomic commit (item 7) 7 to 10;
  0.9.0 itself 8 to 12 (the maintainer: 0.9.0 is cut only when the whole 0.9 plan is
  done; an early tag is his call, Dannan has asked him). The maintainer, 2026-11-16:
  stage 3 waits for the tag; nothing of it is built against an untagged
  Dannan commit, on develop or a branch. The maintainer ruled the same to Dannan: no
  pre-release tag, so the next agreement-bearing tag is 0.9.0 itself.
  Dannan tells Mimir when it is cut, and sends the AcceptorStore interface
  for Mimir's review once value-less prepare and deltas settle it, before
  the tag (Mimir's store is its first real user).
- Reviewed 2026-11-16, Dannan's "Value-less prepare and deltas" (058a568):
  Mimir chose (a), value-less prepares with the reduction deciding from
  summaries and fetching only diverged names (preferably owner bytes "since
  a summary"), not (b), which would bring every Mimir value back into
  prepares; summary per name, 64 bytes (a merge-frontier digest, length,
  type); apply in place, apply(slot, name, base_ballot, new_ballot, delta,
  done(applied | need_whole)), not bytes in and out; value() may stay bytes
  (DUMP form). Dannan took all five into its design (develop `930b088`,
  "Mimir's review (2026-11-16), taken") and builds once Kvasir has answered
  for ranges.
- Dannan's reconfiguration is built on its `feature/agreement-reconfiguration`
  (`c524a28`, not merged): per-slot records, transfer(), settle, a
  caller-named slot, a slot-table fingerprint per request, hint(space, slot).
  The AcceptorStore interface changed (load(slot, names, every, done),
  keep(slot, held, done), optional records()): read `agree/agreement.h` there
  before building Mimir's store.
- Pinned to Dannan 0.8.5 (ABI of 0.8.0 kept). Dannan's 0.9 is on its
  develop. A build server's Dannan checkout without GitHub credentials takes a new tag
  by bundle: `git bundle create <file> refs/tags/<tag> ^<previous>` in
  ../dannan, then `git fetch <file> "refs/tags/<tag>:refs/tags/<tag>"`.

## The differential gate

`Differential.*` in `mimir/test/differential_test.cpp`, run by hand on
node-d or node-a (Valkey 8.1.1 on both, service disabled):

```
LC_ALL=C valkey-server --port 7379 --bind 127.0.0.1 --save "" --appendonly no --daemonize yes
./mimir-node --listen 127.0.0.1 --port 7380 --databases 16 --auth-plaintext --seconds 200 & node=$!
MIMIR_DIFF_MIMIR_PORT=7380 MIMIR_DIFF_VALKEY_PORT=7379 ./mimir_tests --gtest_filter='Differential.*'
valkey-cli -p 7379 shutdown nosave; kill $node   # never pkill: the build servers run mimir.service
```

`LC_ALL=C` because Valkey's SORT ALPHA collates by its environment's locale.
Valkey's own answers are the expected values: probe it first, write the case,
then the code.

## How work is done

- No builds or test runs on the Mac. Every build is on the build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends; a note before any install). Sources
  go by rsync (`--no-o --no-g`, excluding `build*`) to `/root/mimir-work/mimir`
  with Dannan beside them; `chown -R root:root` after the sync. Build:
  `MIMIR_CI_JOBS=2 nice -n 19 tools/ci.sh release`, or an incremental
  `cmake --build build-ci/release -j2`.
- The Lua packages (liblua5.1-0-dev, lua-cjson-dev, lua-bitop-dev,
  lua-cmsgpack) are on node-d and node-a; clang and libFuzzer on node-a.
  Fuzz targets on the build servers: `-DMIMIR_FUZZ_SANITIZERS=fuzzer,undefined` (ASan
  under clang's libFuzzer would not start there; g++'s ASan runs, Dannan
  reports).
- TSan (the maintainer, 2026-11-16): on the Mac only when hunting a specific bug. Every
  build server now boots the 4K-page kernel, so g++'s TSan runs on all four and
  clang++-19's on node-a, node-b and node-c. No sanitizer builds on node-d
  (4 GB; it ran out of memory on 2026-11-16): use node-b or node-c (15 GB),
  and check `free -m` first.
- Commit when a step's gate passes; push `develop`. Read the completed hosted
  CI runs, not only the newest (superseded runs are cancelled); push no more
  until a run completes when a fix awaits confirmation.

## Running: the claim board's registrar (the maintainer, 2026-11-16)

- Deployed 2026-11-16: mimir.service (develop `79b62bb`) on all four build servers,
  port 6390 (node-a, node-b, node-c at 00:33Z; node-d at 00:35Z after the maintainer
  power-cycled it). Installed from the tarball of node-a's build, the Lua
  runtime packages, `tools/deploy/`, and secrets.yaml streamed from the
  passwords file. Passwords:
  `~/.claude/mimir-registrar.yaml`. Accepted over the LAN (NOAUTH,
  registrar writes node:* only, reader reads, notifications heard).
- To upgrade: build on a build server, package bin/mimir-node and lib/ (libmimir,
  libdannan), unpack to /opt/mimir, `systemctl restart mimir` (entries come
  back with the next refresh).
- Every build server runs a mimir-node service now: a test node (the differential's)
  is stopped by its own PID, never `pkill -x mimir-node`, which also stops
  the service's (it did, on node-a at 01:17Z and node-d at 03:05Z,
  2026-11-16). The unit is `Restart=always` since, so a node a signal stops
  comes back; checked by SIGTERM on node-d.

- The maintainer: the claim board keeps a registrar of the build machines in Mimir, "a
  good test for it"; mimir-node runs on all four build servers ("it gets better func,
  you get a test"); plaintext AUTH on the lab LAN (ruling 28's
  `auth.plaintext`, this deployment only); the registrar ships as claim-board
  0.4.0. Spec: `../stack-coordinator/TODO/2026-11-16-claim-board-registrar.md`.
- Mimir's part: install mimir-node from develop on each build server as a systemd
  service on port 6390, users `default off`, `registrar` (writes `node:*`),
  `reader` (reads), in `/etc/mimir/secrets.yaml`; the passwords handed to the
  claim-board session in a file under `~/.claude`. Four independent
  nodes until stage 3; each build server writes its entry to all four, readers read
  their own first. The Mimir session upgrades them as develop moves.

## Waiting on the maintainer


- The shard count's cap of 8192 (Dannan's advert is at most 1 KiB):
  recommended to accept as the upper bound of "configurable" (ruling 15).
- `decided` for a merging write (the design's "Durability", Proposed): a
  majority holds it in memory before the answer. Asked when stage 3 is reached.

## Proposed, not yet put to the maintainer

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
