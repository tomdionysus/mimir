# Mimir - Active Work

Last updated: 2026-11-22. `COMPLETED.md` is the record, with gate counts. The
design is `TODO/2026-11-15-mimir-design.md`: the maintainer's rulings (1 to
44), the two classes of command, agreement in Dannan, durability, and the
order of the stages.

## Resume here

- 0.1.0 is released: main and develop at `7da897b`, annotated tag `0.1.0`,
  hosted CI green, the release badge showing it. develop has since gained
  documents only.
- Dannan is pinned at 0.10.1 (tag `840b08f`), fetched from GitHub on a
  build server, or `libdannan-dev` for the package.
- Coverage: 97.9% of lines on develop (the coverage feature, squashed
  2026-11-22; COMPLETED.md has its fixes and gate). What is left is mostly
  lines gcov misattributes and defensive branches; keep it there as code
  lands (the maintainer, 2026-11-20: "~100% code coverage on all code").
- The maintainer, 2026-11-22: after the coverage squash, the Valkey gaps
  next (item 1), with no release first.
- The registrar on the four build servers runs the 0.1.0 package (see
  "Running").
- The maintainer resumed work on 2026-11-22 ("continue autonomously"); the
  merged feature branches were deleted locally and on the remote that day.

## Next, in order (none waits on anyone)

1. **Found by the probes, not yet done** (the maintainer, 2026-11-22: these
   next). Each is Valkey 8.1.1's behaviour Mimir lacks, to be probed and
   tested first as the coverage work was (`build/cov-probe*.sh`; the kept
   `mimir-dev:trixie` image on a build server, the tree rsynced with
   `--checksum` and no times, `build/dev-run.sh`; node-d's resolver drops
   lookups now and then, so a container there is given public resolvers):
   XINFO STREAM key FULL [COUNT n]; a stream node closed at
   stream-node-max-bytes (4096) as well as by entries, which DUMP's bytes
   and approximate trims show; LATENCY HISTOGRAM's per-command histograms
   (Valkey tracks them by default); redis.setresp(3) changing a script's
   conversions; PFDEBUG (also item 4).
2. **CONFIG SET cluster-wide**: a setting that must hold on every node
   (notify-keyspace-events first) becomes cluster state by agreement, as ACL
   users and function libraries did (no special node).
3. **Stage 3's gates** (3.10 below): the differential against a cluster of
   three; Dannan's SimNodes with Mimir's actors under faults, checked with
   `dannan::sim_checkers` (exported since Dannan 0.10.0).
4. Small things owed from stage 2: MEMORY STATS; PFDEBUG; acllog-max-len
   fixed at 128; ACL DRYRUN checks a subcommand against its container's
   arity.
5. In the next minor (0.2.0), never mid-release: the two flags that break
   the stack's settings rule (Dannan's `docs/config.md`: key `a.b` is
   `--a-b` and `MIMIR_A_B`) become `--listen-host` (now `--listen`) and
   `--listen-port` (now `--port`).

## Waiting on others

- Dannan: the register layer (routes 600 and 601, operation ids and
  `version_of` move onto it; catch-up becomes its exchange); AUTH throttled
  per client address on its keyed map; placement by transfer and replica
  sets kept whole (stage 3 steps 3, 4 and 6); its atomic commit across
  replica sets (its item 26), which every multi-key write waits on (ruling
  35; Mimir's proposal is `TODO/2026-11-15-cross-slot-commit-proposal.md`);
  the peer-witnessed high-water mark (stage 4); its HTTP core (the
  dashboard); a slot table whose count can change (ruling 41).
- Dagda: contract v2, for persistence (stage 5, ruling 43: Mimir writes no
  durable file of its own).
- Lugh: principal events and status scope, for the Lugh plugin.
- The maintainer: `decided` for a merging write (the design's
  "Durability", Proposed), to be asked when stage 4 starts.
- Until replica sets ship, Dannan's operator rule holds: never retire a
  genesis member while slots are still on genesis.

## Stage 3: what is left

Done so far (each in `COMPLETED.md`): the register and the acceptor store
(Dannan's MemoryStore since 0.1.0); the node as a dannan::Node, a lone one a
cluster of one; single-key writes by agreement; commit notices;
transactions and WATCH over one key; blocking pops on one key; scripts on
their one declared key; pub/sub and keyspace notifications across the
cluster; catch-up after a restart, paged by name; strict reads (ruling 37);
expiry by agreement; ACL users (ruling 27) and scripts and function
libraries (ruling 39) by agreement; MIMIR NODES (ruling 40).

Still to do:

3. **Routing** (ruling 38): a node outside a key's set forwards the command
   to a replica (Routes::call); never MOVED; read-your-writes when a write
   was forwarded. Built with placement.
4. **Membership**: placement moves slots by transfer() as nodes join and
   leave; replicas come from the slot's record, not the genesis set.
5. **change_deltas** on a local base, so an agreement write to a large
   collection is not O(n) at the proposer (MemoryStore's Codec can apply
   deltas now).
6. **Cluster-wide keyspace commands** (ruling 33), looked at 2026-11-21:
   while every slot is on the genesis set and commit notices go to every
   genesis node, a genesis node's view already answers KEYS, SCAN, DBSIZE
   and RANDOMKEY for the whole cluster (stale only as ruling 5 allows).
   Asking a replica slot by slot matters only on a node that is not a
   replica, which cannot serve a plain GET either. So it is built with
   routing and placement. The flushes wait for the atomic commit.
9. Smaller gaps: a script that only reads but declares its key counts as a
   write for WATCH; per-channel interest for pub/sub (every publication goes
   to every node today); ACL SETUSER and DELUSER inside MULTI are refused
   in a cluster; a flush, SWAPDB and SORT ... STORE name the keys of this
   node's view until the atomic commit.
10. **Gates**: the differential against a cluster of one and of three;
    SimNodes under faults, checked for linearizability (SimNodes needs
    `DANNAN_TEST_TEMP_ROOT` defined by Mimir's tests); then the registrar
    moved to one four-node cluster.

Later stages: 4, the merge class and the heal rule; 5, persistence on
Dagda; 6, eviction, then the Lugh plugin (links `lir::client`).

## What stage 3 rests on

Settled with Dannan (the record is in `COMPLETED.md`, "The Dannan 0.9.0
reviews"):

- Agreement is per slot. Mimir's shard is Valkey's slot (CRC16 of the hash
  tag or key) mapped onto the shard count in contiguous ranges, so tagged
  keys share a shard. A change names one key until the atomic commit.
- A key's register is named by its database and key. Its value is Mimir's
  record: the DUMP payload, the absolute expiry, the recent operation ids
  with their replies, and a serial.
- Every change is repeatable: it carries an operation id (node, durability
  epoch, counter), and a record already holding the id answers the
  recorded reply and changes nothing.
- The store is in memory and is not ready after a restart until caught up.
- A write runs speculatively on a scratch keyspace holding only its key,
  with a fixed clock and a seeded random source and its side effects held
  back; on commit the record is installed and the side effects run once.
- Each node's live keyspace is its committed view (ruling 32), reached by
  best-effort commit notices (type 600), installed only if newer.
- The replicas are the genesis set at version 0 until placement moves slots.

## How work is done

- No builds or test runs on the Mac. Builds run on the build servers under
  a claim: `claims_list`, `claim_take` with the cores the `-j` needs, `nice
  19`, `claim_release` the moment it ends; when none are free, `claim-board
  wait` in the background, never polling. Book cores only for a job ready
  to start, never ahead (the maintainer, 2026-11-20), and hold none idle.
  Work that other projects wait on goes first; yield between steps when
  asked.
- Each run is a throwaway `debian:trixie` container on a build server,
  pinned to the claimed cores (`--cpuset-cpus`), so nothing is installed
  on the host and nproc matches the claim. Mount a directory made mode 777
  (the servers remap container users); start it with `--cap-add SYS_PTRACE
  --security-opt seccomp=unconfined` for the sanitizers. The driver used
  for 0.1.0 is `build/gate-0.1.0.sh` (not in git): it installs the build
  packages, clones Mimir at a commit from GitHub, runs `tools/ci.sh` steps
  one at a time (`ONLY="..."` to choose, `JOBS=n`), logs each to `/t/logs`,
  records `<step> exit=<code>` in `/t/results.txt`, skips a step already
  recorded, and pauses between steps while `/t/pause` exists (to yield).
- `tools/ci.sh` steps: check, release, clang, asan, tsan, coverage, client,
  client-asan, fuzz (`MIMIR_FUZZ_SECONDS`), package (installs, checks and
  purges the deb: only in a container or runner), tidy. The sanitizer and
  fuzz steps build Dannan instrumented and fail unless libdannan carries
  the sanitizer's symbols. `tools/differential.sh <build dir>` runs the
  differential against Valkey 8.1.1 (Debian's valkey-server, installed in
  the container), one fresh node per case.
- Sanitizers run once per completed feature, before its squash onto
  develop, or to hunt a bug nothing else finds. A gate runs once; a result
  that changes between runs is a defect to investigate. A fix to a gated
  tree means the gate runs again on the fixed tree.
- node-d has 3 to 4 GB: `-j1`, and no sanitizer builds there.
- Features go on `feature/<name>` off develop, squashed onto develop as one
  commit each; documents and small fixes go to develop directly. Hosted CI
  runs on develop and main only, not for documents alone: check, release on
  x86-64 and arm64, clang, client, package on x86-64 and arm64, tidy and
  coverage; CodeQL on main and weekly. Read every completed run after a
  push and fix a red one first. Check the whole tree's format
  (`clang-format-19 --dry-run -Werror`) before committing.
- A test node on a build server is stopped by its own PID, never `pkill`:
  every build server runs mimir.service. Stop every server and node a run
  started.
- ACL's table (`mimir/command/acl_table.inc`) is generated from Valkey
  8.1.1's COMMAND INFO by `build/acl-gen.py` (not in git): `COMMAND LIST`
  from mimir-node, Valkey's `-3 --json COMMAND INFO` of those into
  keyspecs.json, then `acl-gen.py keyspecs.json`.
- A release (the maintainer, 2026-11-20): docs rationalised (README on
  installation and use), the debs built, the full gate once on the cluster
  (sanitizers and fuzzing included), develop merged to main, tagged x.y.z
  (annotated, no `v`), both pushed, the release badge kept. Only minor or
  patch versions are bumped. Before a repin, read the dependency's
  changelog and its "Adopting" notes, and record what changed and what did
  not in the repin's commit or here.

## Running: the claim board's registrar

- mimir.service on all four build servers, port 6390, four independent
  nodes, from the mimir 0.1.0 package with libdannan0.10 0.10.1 (since
  2026-11-21). Users: `default off`, `registrar` (writes `node:*`), `reader`
  (reads). Plaintext AUTH on the build servers' LAN (ruling 28's `auth.plaintext`).
  Passwords: `~/.claude/mimir-registrar.yaml`, never in the repository.
  Spec: `../stack-coordinator/TODO/2026-11-16-claim-board-registrar.md`.
- The deployment's own `/etc/mimir/config.yaml` (notifications, port 6390,
  plaintext AUTH, no `cluster` section: a cluster of one, its state in the
  unit's private /tmp) and `secrets.yaml` (600, owned by mimir) are kept
  over the package's. The old `/opt/mimir` build, its unit and the old
  `/etc/mimir` are in `/opt/mimir/before-0.1.0` for rollback.
- Check it as a client: `build/registrar-check.sh <host>...` (not in git)
  AUTHs as both users without printing the passwords, checks a wrong
  password is refused, and writes and reads back a `node:` key. The
  reader's ACL does not grant MIMIR NODES (NOPERM), as configured.
- To upgrade: build the release's debs from its tag in a container, post a
  board note, then one server at a time, under a claim:
  `build/registrar-upgrade.sh` (not in git) installs with
  `--force-confold`, gives secrets.yaml to the mimir user, enables the
  service and shows its first lines; `rollback` undoes it. Never purge: the
  package's purge deletes `/etc/mimir`, the users with it.
- It becomes one four-node cluster when stage 3's gate passes and Dannan's
  atomic commit lands (its writes must not be refused by ruling 35): each
  server's state made with `mimir-node --cluster-state DIR --init-state`,
  the four ids in every node's `cluster.genesis`.
