# Mimir - Active Work

Last updated: 2026-11-16 (ruling 31, the registrar). `COMPLETED.md` is the record, with
gate counts. The design is `TODO/2026-11-15-mimir-design.md`: Tom's rulings
("Ruled", 1 to 31), the two classes of command, agreement in Dannan,
durability, and the order.

## Resume here

- State: `develop` pushed (the remote is public since 2026-11-15). Last gate:
  release 310 passed, differential 33 of 33 (corvus-jupiter, whose build tree
  is `/root/mimir-work/mimir/build-ci`; uranus is back but 4 GB). Hosted CI
  green at `6f4cc54` (five jobs since the sanitizer jobs were removed on
  2026-11-17; its clang job catches thread-safety annotations the Pis' GCC
  gate cannot: read every completed run).
- Single-node work still owed (none waits on anyone): MEMORY STATS; PFDEBUG.
- The single-node stage is complete: every Valkey type and command family,
  RESP2 and RESP3, pub/sub, transactions, server commands, configurable
  databases, scripting (EVAL, FUNCTION, Valkey's Lua libraries and read-only
  sandbox), keyspace notifications, DUMP and RESTORE, SORT, GEO, LCS, and ACL
  with AUTH (rulings 12, 27, 28).
- ACL's table (`mimir/command/acl_table.inc`) is generated from Valkey
  8.1.1's COMMAND INFO; the generator is `build/acl-gen.py` here and in
  `/root/mimir-work/acl/` on jupiter (and uranus). A new command's row: run
  mimir-node, `COMMAND LIST` into mimir.txt, Valkey's `-3 --json COMMAND
  INFO` of those into keyspecs.json, then `acl-gen.py keyspecs.json`. Small gaps owed: acllog-max-len is fixed at 128;
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
- Dannan's estimates (2026-11-16, in sessions): change(), read(), forget()
  with the caller-named slot, transfer(), settle and the fast path merge to
  Dannan's develop this session; value-less prepare and deltas change
  AcceptorStore once more, about 1 session, after which the interface should
  hold; the hybrid clock 4 to 6; cross-slot atomic commit (item 7) 7 to 10;
  0.9.0 itself 8 to 12 (Tom: 0.9.0 is cut only when the whole 0.9 plan is
  done; an early tag is his call, Dannan has asked him). Tom, 2026-11-16:
  stage 3 waits for the tag; nothing of it is built against an untagged
  Dannan commit, on develop or a branch. Tom ruled the same to Dannan: no
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
  for ranges. Built on Dannan's `feature/valueless-prepare` (`5a09792`, not
  on develop): Reduce returns Reduced{value, need}, Need{answer, since};
  load() takes `values`; keep() leaves value and summary when has_value and
  no value; new value(slot, name, ballot, since, done) by exact ballot;
  fetches in 1 MiB pieces. Deltas (part 4) and transfer (part 5) to come.
  Asked Dannan (2026-11-16): how value() says "ops past since" versus
  "whole" when the holder no longer knows the digest's frontier. Taken: the
  fetch answer carries `past`; a store may always answer whole.
- Reviewed 2026-11-16, Dannan's "Found in building part 4" (`ca7ad31`):
  deltas go into keep() (Held carries delta and base; Kept{kept,
  need_whole}), not a separate apply(). Mimir agreed, with four points:
  - nothing is kept when need_whole is non-empty;
  - a keep() already at new_ballot isn't applied again;
  - deltas and ballots are kept in one append;
  - a delta the value can't take answers need_whole.
  Mimir's keep() checks every delta, then applies them all under the
  database lock.
- Reviewed 2026-11-16, the final AcceptorStore (Dannan
  `feature/valueless-prepare` `1c624d3`, `dannan/agree/agreement.h` and
  `caspaxos.h`). The store side is accepted:
  - load(slot, names, every, values, done);
  - keep(slot, held, done(Kept{kept, need_whole}));
  - value(slot, name, ballot, since, done(optional<Fetched{bytes, past}>));
  - accepts may be ballot-only (`held`), and compact forms carry the base's
    summary.
  One point on cost, put to Dannan: Change takes and returns whole bytes and
  the base is always fetched whole, so an agreement write to a large
  collection is O(n) at the proposer. Asked for (a), a local-holder Change
  with lazy whole values (after 0.9.0 is fine). Mimir plans (b), sending
  agreement writes to a node in the slot's set, and asked whether a fetch
  prefers self. Dannan's answer: it does (fetch_bases puts self first and
  reads it locally), so (b) needs nothing more from Dannan. (a) is recorded
  as Dannan's first item after 0.9.0, and its API comes to Mimir before it
  is built. Tom then moved it into 0.9.0 as part 6 (Kvasir asked for the
  same thing).
- Reviewed 2026-11-16, change_deltas(..., DeltaChange(span<const Base{ballot,
  summary, has_value, local}>) -> Deltas{deltas, result}): it fits, with
  three asks:
  1. Deltas can decline: the base isn't local, or Mimir's live state moved
     off it. No accept is sent, and Mimir retries on change().
  2. Deltas carries the summary each delta yields, and the fast path checks
     ballot and summary. Commutation isn't relied on, since a reply
     computed from the base doesn't survive a merge (locks).
  3. DeltaChange is never run under Dannan's locks. need_whole acceptors
     are caught up by an identity change() (asked Dannan to confirm).
  Dannan took all three (2026-11-16):
  - `Deltas{deltas, result, decline, summaries}`; a decline ends with
    why "declined";
  - a name with no summary takes no fast path;
  - DeltaChange runs on the agreement's strand with no lock held;
  - identity change() confirmed for catch-up.
  Mimir's stage 3 plan:
  - agreement writes run on a node in the key's set, through change_deltas
    when the base is local, otherwise through change();
  - the fast path is used only with the summary.
- Reviewed 2026-11-17, Dannan's `TODO/2026-11-17-agreement-review.md`
  (`feature/attempt-timeout`, `0702313`), which found that a change can be
  applied twice and a multi-name change can be half taken. Mimir's answer:
  - Every Mimir change is repeatable, through its own operation id (the
    connection's dot) and the recorded result kept in the value. A change
    finding its id applies nothing and answers the recorded reply. Unknown
    outcomes are resolved by rerunning the same tagged change.
  - The id list is bounded by count until the hybrid clock; a write is
    refused rather than evicting a live id.
  - Asked that ancestry (16 ballots per key, persisted) be optional per
    space, or compactly encoded.
  - The reduction takes only merge ops, never another answer's agreement
    change.
  - Multi-name changes: option 2, plan item 7's atomic commit, in a slot
    as across slots. About one extra round trip for multi-key MULTI, MSET,
    RENAME, LMOVE and the like; single-key commands stay one round.
  - Asked Dannan to refuse change() of several names until item 7 lands.
  Taken and merged on Dannan's develop (`ea84a31`, 2026-11-17):
  - change(), change_deltas() and forget() throw for more than one name
    until item 7; read() may name several;
  - ancestry is optional (a store without it loads it empty), so Mimir
    keeps none;
  - a repeatable change is re-run on the retry's own base.
  Mimir's store keeps no ancestry, and every change carries its operation
  id.
- Reviewed 2026-11-17, Dannan's simulation transport seam
  (`TODO/2026-11-17-sim-transport-seam.md`, `feature/sim-durability`): the
  call seam (option 2) suits Mimir, and Mimir needs no session layer in the
  world.
  - Mimir's actors use call(), broadcast() and send() with faults, and
    node adverts through membership.
  - Mimir's RESP listener and MIGRATE stay outside the world. Actors drive
    Database::execute directly.
  - Asked for: per-node clock offset and drift; restart with the store
    kept or wiped; matrix partitions with a heal at a chosen tick.
  Built on Dannan's develop (`d50bb67`, `dannan/test/fixtures/sim/nodes.h`):
  - SimNodes(world, n, faults, skew_ms, drift_ppm) and restart(i, wipe);
  - SimNetwork::partition and heal().
  A wipe takes a new identity. Asked for a wipe that keeps the id with a
  new epoch, for the restart rule. Taken (`feature/sim-agreement-nodes`,
  `ccbe3f1`):
  - restart(i, Wipe::store | Wipe::identity | Wipe::nothing);
  - SimNodes::host(i, node, lost) runs after every start, which is where
    Mimir serves its space and, when lost, keeps its store not ready until
    catch_up().
  The example is `SimNodes.AgreementOverRealMembershipStaysLinearizableUnderFaults`.
- 2026-11-17: Dannan 0.9.0 is to be cut tonight (Tom), from develop
  `5276c24`. Mimir's answers on the open items:
  - can wait for 0.10.0: atomic commit across sets (multi-key commands
    are refused in cluster mode until it lands), the peer-witnessed
    high-water mark, and the exported fuzz harness;
  - don't use: DTLS/UDP and loader promotion.
  Asked Dannan to confirm the tag holds change_deltas (decline, summaries,
  repeatable), the final AcceptorStore, the sim fixtures exported for
  linking, and max_slots 8192.
  All five were confirmed on develop `df4b2d8`, and the tag waits on Tom's
  word.
  - The checkers link as `dannan::sim_checkers` (in dannanConfig), and the
    fixture headers install under `dannan/test/fixtures/sim/`.
  - SimNodes needs `DANNAN_TEST_TEMP_ROOT` defined by Mimir's tests (a
    directory in the build tree).
  - CHANGELOG 0.9.0 names the ABI and API changes from 0.8.x.
  - Moved to 0.10.0: atomic commit, DTLS/UDP, the high-water mark, loader
    promotion, and the fuzz export.
  On the tag: move the pin to 0.9.0, bundle the tag to the Pis, then begin
  stage 3.
- Dannan 0.9.0 was tagged 2026-11-17 (main `59b7abc`). Mimir's pin moved
  to 0.9.0 the same day: release 310 passed and differential 33 of 33, on
  uranus at -j1. Uranus has the tag; the other Pis take it by bundle when
  they next build.
- Dannan's reconfiguration is built on its `feature/agreement-reconfiguration`
  (`c524a28`, not merged): per-slot records, transfer(), settle, a
  caller-named slot, a slot-table fingerprint per request, hint(space, slot).
  The AcceptorStore interface changed (load(slot, names, every, done),
  keep(slot, held, done), optional records()): read `agree/agreement.h` there
  before building Mimir's store.
- Pinned to Dannan 0.9.0 (from 0.8.5, 2026-11-17). Dannan's 0.9 is on its
  develop. A Pi's Dannan checkout without GitHub credentials takes a new tag
  by bundle: `git bundle create <file> refs/tags/<tag> ^<previous>` in
  ../dannan, then `git fetch <file> "refs/tags/<tag>:refs/tags/<tag>"`.

## The differential gate

`Differential.*` in `mimir/test/differential_test.cpp`, run by hand on
corvus-uranus or corvus-jupiter (Valkey 8.1.1 on both, service disabled):

```
LC_ALL=C valkey-server --port 7379 --bind 127.0.0.1 --save "" --appendonly no --daemonize yes
./mimir-node --listen 127.0.0.1 --port 7380 --databases 16 --auth-plaintext --seconds 200 & node=$!
MIMIR_DIFF_MIMIR_PORT=7380 MIMIR_DIFF_VALKEY_PORT=7379 ./mimir_tests --gtest_filter='Differential.*'
valkey-cli -p 7379 shutdown nosave; kill $node   # never pkill: the Pis run mimir.service
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
  Fuzz targets build with clang 19 on jupiter, saturn or neptune, with the
  default `fuzzer,address,undefined` now that the kernels have a 48-bit
  address space (on a 39-bit one, `-DMIMIR_FUZZ_SANITIZERS=fuzzer,undefined`).
- Sanitizer runs (ASan, UBSan, TSan) only before a release, or to hunt a
  named bug that cannot be pinned any other way. Never as a step's gate or
  a routine check (Tom; restated to every session 2026-11-17). Name the
  release or the bug before starting one. Mimir's gate is release plus
  differential. Hosted CI has had no asan or tsan job since 2026-11-17
  (Tom: "Remove them"); it keeps check, the two release builds, clang and
  coverage.
- Sanitizers (2026-11-16): every Pi boots a 4K-page, 48-bit-VA kernel
  (6.18.62-v8-va48+), so ASan, UBSan and TSan run on all four with g++, and
  with clang 19 on jupiter, saturn and neptune (uranus has no clang). TSan
  on the Mac only when hunting a specific bug (Tom). No sanitizer builds on
  uranus (4 GB; it ran out of memory on 2026-11-16): saturn or neptune
  (15 GB), `free -m` first.
- Commit when a step's gate passes; push `develop`. Read the completed hosted
  CI runs, not only the newest (superseded runs are cancelled); push no more
  until a run completes when a fix awaits confirmation.

## Running: the claim board's registrar (Tom, 2026-11-16)

- Deployed 2026-11-16: mimir.service (develop `79b62bb`) on all four Pis,
  port 6390 (jupiter, saturn, neptune at 00:33Z; uranus at 00:35Z after Tom
  power-cycled it). Installed from the tarball of jupiter's build, the Lua
  runtime packages, `tools/deploy/`, and secrets.yaml streamed from the
  passwords file. Passwords:
  `/Users/tom/.claude/mimir-registrar.yaml`. Accepted over the LAN (NOAUTH,
  registrar writes node:* only, reader reads, notifications heard).
- To upgrade: build on a Pi, package bin/mimir-node and lib/ (libmimir,
  libdannan), unpack to /opt/mimir, `systemctl restart mimir` (entries come
  back with the next refresh).
- Every Pi runs a mimir-node service now: a test node (the differential's)
  is stopped by its own PID, never `pkill -x mimir-node`, which also stops
  the service's (it did, on jupiter at 01:17Z and uranus at 03:05Z,
  2026-11-16). The unit is `Restart=always` since, so a node a signal stops
  comes back; checked by SIGTERM on uranus.

- Tom: the claim board keeps a registrar of the build machines in Mimir, "a
  good test for it"; mimir-node runs on all four Pis ("it gets better func,
  you get a test"); plaintext AUTH on the lab LAN (ruling 28's
  `auth.plaintext`, this deployment only); the registrar ships as claim-board
  0.4.0. Spec: `../lir-develop-cluster/TODO/2026-11-16-claim-board-registrar.md`.
- Mimir's part: install mimir-node from develop on each Pi as a systemd
  service on port 6390, users `default off`, `registrar` (writes `node:*`),
  `reader` (reads), in `/etc/mimir/secrets.yaml`; the passwords handed to the
  claim-board session in a file under `/Users/tom/.claude`. Four independent
  nodes until stage 3; each Pi writes its entry to all four, readers read
  their own first. The Mimir session upgrades them as develop moves.

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
   gate. Done.
3. The cluster by agreement only. Dannan 0.9.0 is tagged and pinned:
   single-key commands first; multi-key ones are refused in cluster mode
   until 0.10.0's atomic commit.
4. The merge class and the heal rule. Waits on Dannan 0.9.0's hybrid clock,
   and on 0.10.0's peer-witnessed high-water mark (Tom, 2026-11-17).
   Without key custody, the clock's ceiling is a plain durable file, so a
   restored state directory could issue stamps again. 0.10.0 closes that:
   a node issues no stamp until its clock passes the highest stamp a
   majority of peers report from it.
5. Persistence, `persisted` and `replicated`, checkpoints. Waits on Dagda's
   contract v2.
6. Eviction, then the Lugh plugin. The plugin waits on Lugh's principal
   events and status scope; it links `lir::client` (Lir's develop at
   `6b23268`, on Dannan 0.8.3, until Lir's first tag).
