# Mimir - Completed

The record, newest first. Each entry says what landed, with its gate counts.

## 2026-11-20: One path: every write by agreement, on every node (the maintainer: no single-node case)

- The maintainer, 2026-11-20: "there shall be NO special case code for a single node."
  Built on `feature/one-path`, squashed onto develop.
- mimir-node: a lone node no longer votes at once after a restart; every
  node that restarts catches up first.
- `command::Agreements` gained change_keys (several keys at once) and
  several_keys(), the cluster's capability. cluster::Agreements answers
  false and refuses, until Dannan's atomic commit across replica sets
  (item 26); the database refuses a multi-key write with ruling 35's words
  where the cluster cannot take it. That is a capability of the cluster, not
  a node's.
- Database: agreements are required; the no-cluster execute path is gone.
  A write names its keys (names_locked):
  - MOVE's and COPY ... DB's other database;
  - every key of this node's view for FLUSHALL, FLUSHDB and SWAPDB;
  - an EXEC's queue and watches across SELECT;
  - a script's declared keys;
  - SORT ... STORE's pattern keys.
  It is speculated over a scratch keyspace per database holding only those
  keys (speculate_keys). db_locked and swap_locked answer the scratch while
  speculating. SORT without STORE runs on the view as a read.
- MIGRATE removes each moved key by a conditional agreement (only while it
  holds what moved), and answers once the removals are decided.
- Fixed on the way, each found by a test that now passes through agreement:
  - ZUNIONSTORE, ZINTERSTORE and ZDIFFSTORE named only their destination
    (the table now names their sources);
  - CLIENT REPLY OFF and SKIP did not silence a reply given later by
    agreement;
  - CLIENT UNBLOCK could not end a pop parked by agreement;
  - a transaction's reads were not tracked for its caller;
  - a woken pop on a key now of another type answered WRONGTYPE (it now
    waits on, and is retried only when a key it waits on is written again,
    by version);
  - a large hash, set or sorted set lost its SCAN numbers when a committed
    value was installed (each now adopts the numbers of the value it
    replaces, ruling 30);
  - restoring a key into a speculation announced it "new".
- Tests: every command test runs through agreement on an in-process cluster
  of one that commits several keys at once (`test/fixtures/
  local_agreements.h`; Bed drives it and reruns a postponed command). No
  test was deleted. Five script tests now declare the keys they write
  (ruling 36). Beds that waited for an immediate reply now wait for the
  agreement.
- Gate on node-c (-j1): release 363 passed; ASan 363 and TSan 363,
  no sanitizer report. Differential 233, unchanged in total. 163 replies now
  carry ruling 35's words (from 157), since the sorted-set STOREs now name
  all their keys and are refused as multi-key writes by mimir-node's
  cluster, instead of running wrongly on their destination alone.

## 2026-11-20: MIMIR NODES lists every node's client address (ruling 40)

- The first feature built on a branch (`feature/node-list`) and squashed
  onto develop, under the maintainer's rules of 2026-11-20.
- Each node advertises its name and where it takes clients in its Dannan
  record (advert `mimir.client`: name, host, port, TLS port). Space::
  client_addresses() reads every active node's, this one first; a peer's
  advert is untrusted bytes, and one that does not read is left out.
  mimir-node advertises the listening host, or, when that is a wildcard,
  the cluster's advertised host, or else the address peers dial.
- MIMIR NODES answers each node's id, name, host, port, tls-port and self.
  MIMIR HELP lists it. Every node answers the same list: no special node.
- Mimir's own commands and subcommands now have ACL rows (client|strict,
  mimir, mimir|help, mimir|nodes): a command with no row is never refused
  by ACL, so CLIENT STRICT could not be refused until now.
- Test: ClusterSpace.EveryNodeListsWhereEveryNodeTakesClients (three real
  nodes; the command's reply; an unknown subcommand; a user refused it).
- Ruling 42 (what Mimir calls itself) was checked: HELLO, INFO and
  SERVER_NAME already answer as it says. Nothing to change.
- Gate on node-c (-j1): release 363 passed; differential 233,
  unchanged. Before the squash, ASan and TSan once each: 363 passed under
  each, no sanitizer report.

## 2026-11-20: The varying differential count explained

- The maintainer (2026-11-20): flaky tests are not tolerated. NotificationsCollections
  differed by 29 or 30 replies from run to run. Five runs of that case
  alone, each on a fresh node, gave 29, 30, 29, 29, 30. The varying reply
  is `SPOP s`. In Valkey, `SMOVE s s2 b` leaves `s` holding only "c", so
  its pop is fixed. Mimir refuses that SMOVE (ruling 35), so `s` holds "b"
  and "c", and the pop picks at random (seeded by the operation id).
- A knock-on of a known refusal, not a defect. It goes when multi-key
  writes land (Dannan 0.10.0). No unit test varies.

## 2026-11-20: Pub/sub by prefix (the stack plan's item 7)

- `0ff90ef`. Each node subscribes on Dannan's `mimir.pubsub` notify channel
  by topic prefix instead of to everything:
  - a channel's or shard channel's prefix is its topic ("c:news",
    "s:news");
  - a pattern's is "c:" and its literal part before the first glob
    character ("news.*" gives "c:news.", "*" gives "c:").
- The node keeps a minimal set (no prefix that another covers), so a topic
  matches one subscription at most and is never delivered twice. Dannan
  also folds covered prefixes in its advert.
- PubSub::set_interest tells the fan-out, under the hub's lock, when a name
  gains its first subscriber or loses its last. The fan-out's lock comes
  after the hub's.
- The interest is pushed to every active peer at once (Dannan's subscribe
  with a deadline). A publish made on another node before the push arrives
  goes nowhere (at most once). Valkey's guarantee would need SUBSCRIBE's
  confirmation held until the push is acknowledged, as Lir holds SUBACK. Not
  done; recorded as a gap.
- Tests:
  - ClusterFanout.ANodeWantsThePrefixesItsSubscriptionsNeed;
  - ClusterDatabase.APublishAndANotificationReachEveryNodesSubscribers now
    publishes until heard, since interest is no longer everywhere at once.
- Gate on node-c (-j2): release 362 passed. The cluster and pub/sub
  tests were repeated three times, all passing, because the change is
  timing-sensitive. Differential: 234, matching an earlier run case for
  case (NotificationsCollections varies by one).

## 2026-11-20: Dannan 0.9.2, and the stack audits

- Pin moved from 0.9.1 to 0.9.2, so the stack is on one version. 0.9.2's
  fix is that a node now sends its record to each peer it meets first, so
  an advert set at start reaches peers met later (Mimir's `mimir` capability
  advert is one). The ABI is 0.9.0's. No command path changed, so the
  differential was not rerun. Gate on node-c (-j2, a full rebuild):
  release 361 passed.
- The maintainer asked the stack for two audits, negotiated through the stack's coordinating
  session (`../stack-coordinator/TODO/2026-11-20-cluster-stack-placement.md`):
  duplicated functionality (`TODO/2026-11-20-duplication-audit.md`) and
  special roles and nodes (`TODO/2026-11-20-no-special-node-audit.md`).
  Mimir's answers, given there:
  - catch-up, commit notices, exactly-once ids and ballot versions belong
    in Dannan's agreement;
  - an in-memory AcceptorStore goes in dannan::agree, and Mimir drops
    `mimir/cluster/store.*` when it lands;
  - SHA-1 belongs in Dannan's crypto;
  - pub/sub interest is Mimir subscribing by prefix on Dannan Channels;
  - Mimir needs no lock manager;
  - AUTH is throttled per client address, with the keyed bucket in Dannan;
  - the genesis set as every shard's replicas is the one special set, and
    keeping replica sets whole as nodes join and retire belongs in Dannan.
  Nothing moves until the plan converges and the maintainer decides.

## 2026-11-20: Stage 3: scripts and function libraries by agreement (ruling 39)

- `eb8eb94`. The cluster's own registers share database number 0xFFFF:
  "acl", "functions", and "script:<sha>".
- FUNCTION LOAD, DELETE, FLUSH and RESTORE run on a copy of the agreed
  libraries (none before the first), and the record is FUNCTION DUMP's
  payload of the result (Scripting::library_codes and replace_libraries,
  all or none). Every node's libraries become it at commit or notice.
- SCRIPT LOAD commits the body to its SHA's register. An EVAL's script is
  shared once per node in the background (no waiter).
- An EVALSHA whose SHA this node lacks is parked as postponed, reads that
  register at a majority, caches the body, and runs again. The session
  remembers the SHA so it looks only once. SCRIPT FLUSH clears this node's
  cache only, and a register last loaded before the flush is not brought
  back, so EVALSHA answers NOSCRIPT after a flush, as Valkey's does.
- Inside MULTI, FUNCTION's changes are refused in a cluster, as ACL's are.
- Tests: CommandAgreement.FunctionLibrariesChangeByAgreementAndReachEveryNode
  and ScriptsAreFoundAcrossTheCluster. The test fake counts the system
  registers' proposals apart from keys', so the older script tests still
  count only key proposals; the bed reruns a postponed command, as the
  connection does.
- Gate on node-c (-j2): release 361 passed. Differential, per
  case: 233 differing replies, unchanged.

## 2026-11-20: Stage 3: ACL users by agreement (ruling 27)

- `3dc0af4`. ACL SETUSER and DELUSER commit on one register of their own:
  database number 0xFFFF (no keyspace has it; there are at most 1024) and
  key "acl". Its record's payload is ACL LIST's lines, each length-prefixed
  (Acl::lines and Acl::replace_users, all or none).
- The change runs the ACL handler on a copy of the users as agreed, or,
  before the first change, as this node's settings seed them; meanwhile
  `acl_locked()` answers the copy and `sessions_locked()` none, so nothing
  is closed until the commit.
- On commit, and on each other node at its notice (or at catch-up), the
  node's users become the record (if newer), then it closes the
  connections of a user gone and the subscribers a narrowed user may no
  longer hear (`acl::refused_subscribers`, shared with the local path). The
  proposer's own connection, if closed, closes after its reply.
- Known gaps: inside MULTI they are refused in a cluster; nodes whose
  settings seed different users differ until the first change.
- Test: CommandAgreement.UsersChangeByAgreementAndReachEveryNode (a change
  committed and used here, the same users line for line on another node
  from the notice, a deleted user's connection closed there, a refused
  change and MULTI changing nothing).
- Gate on node-c (-j2): release 359 passed. Differential, per
  case: 233 differing replies; a first run showed 234, the extra one in
  NotificationsCollections, which varies from run to run.

## 2026-11-20: Stage 3: expiry by agreement

- `2d42f55`. When a node's view finds a key expired (a read, or active
  expiry), it announces "expired" to its own subscribers as before, in
  Valkey's order, and proposes the key's deletion: one proposal per key at
  a time, with no waiter.
- The deletion's change deletes only if the agreed record has expired by
  the proposal's issue time. A later expiry committed elsewhere wins: the
  change leaves the record alone, and its commit puts the key back in this
  node's view. The deletion counts as a write for WATCH (serial + 1) and is
  noted with its operation id, so a retry answers as the first attempt.
- A write that finds its key expired takes it out in its own agreement and
  proposes no separate deletion.
- A refused deletion leaves the record holding the key until a later write
  or expiry on some node finds it. Known edge: with clock skew, a node may
  announce "expired" for a key a later EXPIRE elsewhere then keeps.
- Tests: CommandAgreement.AnExpiredKeyIsDeletedByAgreement,
  AnExpiryGivenALaterOneElsewhereIsNotDeleted,
  AWriteToAnExpiredKeyTakesItOutItself; the bed can run on stepped wall
  time. They were written with the code, not run against the old code
  first.
- Gate on node-c (-j2): release 358 passed. Differential, per
  case: 233 differing replies, unchanged.

## 2026-11-20: Stage 3: strict reads (ruling 37)

- CLIENT STRICT ON|OFF (`fed764f`). On, a read that names keys, outside
  MULTI, reads each key from a majority (Agreements::read), installs what
  the majority holds into this node's view, then runs on it; without a
  majority it answers CLUSTERDOWN. A read naming no key (DBSIZE, SCAN) is
  served plain. CLIENT INFO and LIST show ` strict=1` only while it is on.
- Test: CommandAgreement.AStrictReadSeesWhatAMajorityHolds (a value
  committed elsewhere seen, several keys each read, refused without a
  majority, OFF again, a bad argument).
- `tools/differential.sh` (`17cf4d2`): the per-case differential, kept in
  the repository after the wipe took the build server's copy.
- The work tree was put back on node-c after the wipe: Dannan from
  `build/dannan-full-0.9.1.bundle`; liblua5.1-0-dev, lua-cjson-dev,
  lua-bitop-dev and valkey-server 8.1.1 installed (service disabled, a note
  on the board first).
- Gate on node-c (-j2): release 355 passed. Differential, per
  case: 233 differing replies, as before. The runner counts 157 replies
  carrying the ruling 35 refusal and 4 undeclared script keys; the rest
  follow from a refused setup command.

## 2026-11-19: Handoff, and the documents brought up to date

- Where it stands: `develop` at `903f59a`, pushed. Last gate, 2026-11-18 on
  node-a: release 354 passed; the per-case differential shows 233
  or 234 differences, all from rulings 35 and 36 and cases whose setup used
  a refused command. Hosted CI last read green at `494ff46`.
- Not landed: strict reads (ruling 37, CLIENT STRICT ON|OFF) are written
  and in the working tree, neither built nor tested. `ACTIVE.md`, "Resume
  here", lists the files and what is owed.
- Rulings this session: 32 (plain reads serve the last value known
  committed), 33 (keyspace-wide commands are cluster-wide), 34 (a lone node
  is a cluster of one), 35 (multi-key writes refused until Dannan 0.10.0),
  36 (a script writes only the key it declares), 37 (CLIENT STRICT). Hosted
  CI's sanitizer jobs were removed at the maintainer's word.
- The build servers were wiped to bare OS on 2026-11-18 (the maintainer, through the stack's coordinating
  session). mimir.service was kept and checked on all four afterwards:
  active, no restart, every library found, NOAUTH to an unauthenticated
  PING. Its dependency list given before the wipe left out the four Lua
  runtime libraries; the service failed on node-d until the stack's coordinating
  session reinstalled them. The list in `ACTIVE.md` is now complete.
  `/root/mimir-work` went, with the build trees and the per-case
  differential's runner.
- Documents: `ACTIVE.md` rewritten as the plan and the resume note; its
  history of the Dannan 0.9 reviews moved to the entry below; the design's
  stale statements corrected against the rulings; `README.md`,
  `CONTRIBUTING.md` and `CLAUDE.md` brought up to stage 3. No code changed,
  and nothing was built.

## 2026-11-16 to 2026-11-17: The Dannan 0.9 reviews

Moved from `ACTIVE.md` on 2026-11-19. What Mimir asked of Dannan's agreement
while it was built, and what Dannan took. The designs are in `../dannan/TODO/`.

- **Reconfiguration** (2026-11-15, the maintainer approved): agreement records are per
  slot and a change's names share one slot. change(), read(), forget() and
  catch_up() take the slot from the caller; with transfer(), settle, a
  slot-table fingerprint per request and hint(space, slot).
- **Value-less prepare** (Dannan `058a568`): Mimir chose value-less
  prepares, the reduction deciding from summaries and fetching only diverged
  names, over bringing every value back into prepares. A summary per name,
  64 bytes; value() may stay bytes (DUMP form). Dannan took all five points
  (`930b088`). The fetch answer carries `past`, and a store may always
  answer whole.
- **Deltas in keep()** (Dannan `ca7ad31`): deltas go into keep() (Held
  carries delta and base; Kept{kept, need_whole}), not a separate apply().
  Mimir's four points, taken: nothing is kept when need_whole is non-empty;
  a keep() already at the new ballot is not applied again; deltas and
  ballots are kept in one append; a delta the value cannot take answers
  need_whole.
- **The final AcceptorStore** (Dannan `1c624d3`): load(slot, names, every,
  values, done); keep(slot, held, done(Kept)); value(slot, name, ballot,
  since, done(optional<Fetched{bytes, past}>)). On cost, Mimir asked for a
  local-holder change, since a whole-bytes Change makes an agreement write
  to a large collection O(n) at the proposer. The maintainer moved it into 0.9.0; a
  fetch prefers self.
- **change_deltas**: DeltaChange(span<const Base>) -> Deltas. Mimir's three
  asks, taken: Deltas can decline (why "declined"); it carries the summary
  each delta yields, and a name with no summary takes no fast path;
  DeltaChange runs on the agreement's strand with no lock held. An identity
  change() catches up need_whole acceptors.
- **The agreement review** (Dannan `0702313`, which found a change can be
  applied twice and a multi-name change half taken). Mimir's answer: every
  change is repeatable through its own operation id and the recorded reply
  kept in the value; the reduction takes only merge ops; multi-name changes
  wait for the atomic commit, in a slot as across slots. Taken on Dannan's
  develop (`ea84a31`): change(), change_deltas() and forget() refuse more
  than one name; ancestry is optional, and Mimir keeps none; a repeatable
  change is re-run on the retry's own base.
- **The simulation seam** (Dannan `d50bb67`, `ccbe3f1`): the call seam
  suits Mimir, whose actors will drive Database::execute directly, its RESP
  listener and MIGRATE staying outside the world. Asked for and built:
  SimNodes(world, n, faults, skew_ms, drift_ppm); restart(i, Wipe::store |
  Wipe::identity | Wipe::nothing); SimNetwork::partition and heal();
  SimNodes::host(i, node, lost), where Mimir serves its space. The example
  is `SimNodes.AgreementOverRealMembershipStaysLinearizableUnderFaults`.
- **The 0.9.0 cut** (2026-11-17, develop `df4b2d8`, main `59b7abc`): Mimir
  agreed that the atomic commit across sets, the peer-witnessed high-water
  mark and the exported fuzz harness wait for 0.10.0, and uses neither
  DTLS/UDP nor loader promotion. Confirmed in the tag: change_deltas, the
  final AcceptorStore, the sim fixtures exported for linking
  (`dannan::sim_checkers`, headers under `dannan/test/fixtures/sim/`), and
  max_slots 8192.
- **The atomic commit across slots**: Mimir's proposal is
  `TODO/2026-11-15-cross-slot-commit-proposal.md`, reviewed by Kvasir, in
  Dannan's plan for 0.10.0.

## 2026-11-18: A node's own expiries and misses stay its own

- Found reading the fan-out: every node's view raises "expired" (lazy and
  active expiry) and "keymiss" for itself. Carried to every node, a
  subscriber would have heard one expiry once per node.
- Database::publish_events now delivers those two to this node's
  subscribers only (PubSub::deliver). A write's events are still raised by
  the committing node and carried everywhere.
- Test: ClusterDatabase.AnExpiryIsHeardOnceWhereverItIsFound (three nodes
  each find the key expired; a subscriber on one hears it once).
- Gate on node-a (-j1): release 354 passed. The test was not first
  run against the old code: the fault was found by reading.

## 2026-11-18: Stage 3, step 6 (first part): catch-up after a restart

- A member of a cluster of more than one that restarts without its
  acceptor state (in memory until Dagda) catches up before it votes, by
  cluster::Agreements::recover().
  - For each shard it asks a ready peer for the names held there: a new
    user type, 601 `mimir.shard_names`, paged at 1000 names, refused by a
    node still catching up itself.
  - Dannan's catch_up() then reads them at a majority and installs them.
  - Last, it fills its database's view from what it holds (by the commit
    hook, at each record's accepted ballot) and is set ready.
  - A shard that cannot be caught up yet is retried a second later, with
    the next peer.
- mimir-node runs it at start when its acceptors are not ready, and says
  `{"event":"caught_up"}`.
- Store::held(slot) lists a shard's names with their states.
- Test: ClusterDatabase.ANodeRestartedWithoutItsStateCatchesUpBeforeItVotes
  (20 keys written, one of three nodes restarted from its state directory
  with a new epoch: it does not vote, it catches up, serves every key, and
  takes writes again).
- Gate on node-a (-j1): release 353 passed; differential 234 (from
  233, within one run's variation; nothing in the command path changed).

## 2026-11-17: Stage 3, step 8d: pub/sub across the cluster

- `mimir/cluster/fanout.{h,cpp}`: Fanout opens Dannan's notify channel
  `mimir.pubsub` and subscribes this node to every topic.
  - PubSub hands it every local publication: PUBLISH, SPUBLISH, and
    keyspace notifications.
  - Other nodes deliver each to their own subscribers through
    PubSub::deliver, without carrying it on, and skip their own.
  - Each notification is raised once, by the committing node, under its
    own notify-keyspace-events.
- PubSub: publish/spublish deliver locally, then hand the fan-out the
  message once its lock is released. set_fanout and deliver were added.
- Per-channel interest (rather than every topic everywhere) is a later
  refinement.
- Test: ClusterDatabase.APublishAndANotificationReachEveryNodesSubscribers
  (three real nodes).
- Gate on node-a (-j2): release 352 passed; the per-case
  differential shows 233 differences.

## 2026-11-17: Stage 3, step 8c: scripts by agreement

- EVAL, EVALSHA and FCALL that declare one key (or the same key more than
  once) are proposed whole. The script runs speculatively against that key
  alone; a command inside it naming another key or database is refused
  ("names another").
- A script that declares no key runs here, on this node's committed view,
  and may only read. A keyed write inside it is refused ("must declare
  it").
- A script declaring several keys is refused (ruling 35).
- Known gaps:
  - a script that writes a key it did not declare is refused, where Valkey
    on one server runs it (Valkey's documentation asks scripts to declare
    every key);
  - a script that only reads, but declares its key, still counts as a write
    for WATCH;
  - SCRIPT LOAD and FUNCTION LOAD stay node-local, so an EVALSHA on another
    node may answer NOSCRIPT.
- Tests: CommandAgreement covers a script committed on its one key,
  another key refused inside it, several keys refused, and a script with no
  key reading here and refused a write.
- Gate on node-a (-j2): release 351 passed. The per-case
  differential shows 234 differences (from 347).
  - 143 are ruling 35 refusals.
  - 4 are undeclared-key script writes, as above.
  - The rest are knock-ons of those: a case's setup MSET, STORE, RENAME or
    BITOP was refused.

## 2026-11-17: Stage 3, step 8b: blocking pops by agreement

- A blocking write on one key is proposed like any write: BLPOP, BRPOP,
  BZPOPMIN/MAX, BLMPOP and BZMPOP with one key, and XREADGROUP on one
  stream.
- If its attempt finds nothing, the record is left as it was (nothing is
  noted), and the connection parks on the key with the command's own
  timeout. The timeout is read from its arguments; a bad one is refused
  before anything is proposed.
- A commit or notice that writes the key proposes the oldest parked pop
  again, under a fresh operation id, one at a time per key, so the order
  holds. A pop that finds nothing again goes back to waiting.
- A timeout during an attempt waits for its outcome: a popped element is
  delivered, and nothing found answers the timeout's reply.
- A connection that closes mid-attempt leaves the attempt to finish,
  answering nobody.
- CLIENT UNBLOCK leaves agreement waits alone.
- Inside MULTI a blocking command does not wait, as in Valkey.
- Blocking pops over several keys stay refused (ruling 35).
- Found by the differential: the first cut read the timeout from a run
  against an empty keyspace, where XREADGROUP fails with NOGROUP. It is now
  parsed from the arguments.
- Tests: CommandAgreement covers a pop served at once, a pop waiting for
  another connection's write, a timeout during an attempt waiting for the
  outcome, a parked pop timing out, and several keys refused.
- Gate on node-a (-j2): release 350 passed. The per-case
  differential shows 347 differences (from 376): 142 ruling 35, 112 step 8
  (scripts), and the rest their knock-ons.

## 2026-11-17: Stage 3, step 8a: transactions and WATCH over one key

- The record carries `serial`, the count of writes committed to the key.
  An error or an aborted EXEC does not count.
- WATCH in a cluster reads each key's record strictly (Agreements::read, a
  majority) and keeps its serial on the session (`watched_serials`);
  the connection waits for it.
- An EXEC whose writes and watches all name one key is proposed whole.
  - Its speculation runs the queue against the scratch keyspace: run_locked
    and look_up_keys use it, and publish_events, touch, track_reads and
    feed_monitors hold back.
  - The watch check compares the serials inside the agreed change, so an
    EXEC never commits over a write it did not see.
  - The connection's transaction ends when it is proposed.
- An EXEC that only reads runs locally.
- An EXEC naming several keys (or watching another), or one queuing a
  blocking command or a writing script, is refused.
- A result now says whether the write ran: a commit touches its key's
  watchers, trackers and waiters only if it did.
- Tests: CommandAgreement covers a transaction over one key committed whole
  (one proposal), a WATCH aborting after a write elsewhere, a WATCH letting
  the EXEC commit, and several keys refused.
- Gate on node-a (-j2; node-d went down at 22:37Z): release 346
  passed. The per-case differential shows 376 differences (from 380): 136
  ruling 35, 137 step 8 (blocking pops, scripts), and the rest their
  knock-ons.

## 2026-11-17: Stage 3, step 4: commit notices (ruling 32)

- cluster::Agreements sends a commit notice after each commit to the set's
  other active nodes (user type 600 `mimir.commit_notice`, registered before
  the node starts). It carries the database, the key, the version (the
  committing ballot as 40 bytes that order as ballots do) and the record
  when it is at most 64 KiB. The notice is best effort, and a lost one
  leaves a replica stale (ruling 5). The handler decodes untrusted bytes
  with bounds.
- Agreements::Outcome carries the version; Agreements::on_committed hands
  notices to the database.
- Database keeps per key the version its view holds. A commit, its own or
  a notice, is installed only if it is newer, so it is never older than
  what is held and never undone. A notice touches watchers, trackers and
  waiters as a local write does.
- Tests:
  - CommandAgreement: a notice served, an older notice ignored, a notice
    breaking WATCH;
  - ClusterDatabase, three real nodes on loopback with Databases: a write on
    one is read on the others, and 30 concurrent INCRs from all three apply
    once each, with 30 distinct replies.
- Gate on node-d (-j2): release 342 passed. The per-case
  differential shows 380 differences, all of step 3's kinds (from 381).

## 2026-11-17: Dannan 0.9.1

- Pin moved from 0.9.0 to 0.9.1: an advert change is now stamped above
  every copy of the node record already handed out (Lir's finding). Mimir's
  `mimir` capability advert relies on that.
- Gate on node-d (-j1): release 337 passed on 0.9.1. No command path
  changed, so the differential was not rerun.

## 2026-11-17: Stage 3, step 3: writes by agreement

- `mimir/command/agreements.h` is the Agreements interface the database
  proposes through; `mimir/cluster/agreements.{h,cpp}` implements it on
  Dannan's change(). Its operation ids are the node, the epoch and a count;
  every change is repeatable; the replicas are the genesis set for now.
- Database (DatabaseSettings::agreements):
  - A write naming one key parks its connection and is proposed.
  - The Change runs the command speculatively on a scratch keyspace
    restored from the record. Its clock is fixed at the issue time and its
    random source is seeded from the operation id, so attempts agree.
  - Notifications are captured into the result, not published.
  - An operation already applied answers its recorded result.
  - On commit, the record is installed into the live keyspace (this node's
    committed view), and touch, the notifications, invalidations and the
    wake follow. Unknown outcomes are proposed again until 15 s;
    refusals answer -CLUSTERDOWN.
  - Ruling 35: writes of several keys (MSET, RENAME, MOVE, COPY, SWAPDB,
    the flushes, the STOREs) are refused. Blocking commands, EVAL, EVALSHA
    and FCALL, and an EXEC with queued writes are refused until step 8.
  - CLIENT UNBLOCK leaves a proposal alone.
- mimir-node gives the database its Agreements, so a lone node writes
  through agreement too (ruling 34).
- Found by the differential through agreement, and fixed:
  - an "expired" was announced twice (now once, by the live keyspace, when
    the write looks its key up);
  - a restored stream started a new node instead of filling its last one,
    which changed approximate XTRIM (rdb restore; test
    KeyspaceRdb.ARestoredStreamFillsItsLastNode).
- Tests: CommandAgreement (8), with a register stand-in that can lose
  answers and refuse.
- Gate on node-d (-j1): release 337 passed.
- Differential through a lone node, each case on a fresh node (FLUSHALL is
  refused): 381 differing commands.
  - 135 are ruling 35 refusals, and 141 are step 8 refusals.
  - 105 follow from those refusals; each was read, and each depends on a
    refused setup command (MSET, a STORE, BITOP, SMOVE, EXEC or EVAL).
  - No other difference.
  - Known gap: Mimir's DUMP writes no deleted stream entries, so a stream
    whose last node had entries deleted refills it more than Valkey would.

## 2026-11-17: Stage 3, step 2: the node

- `mimir/cluster/space.{h,cpp}`: Space advertises `mimir`, serves the space
  from Store on Dannan's agreement, and founds every shard on the genesis set
  (found()). The shard count is the agreement table's slot count.
- `mimir-node` is a dannan::Node (ruling 34). Its settings are
  `cluster.{key,state,listen,port,advertise,bootstrap,genesis,shards,name,domain}`;
  the cluster port defaults to 16379 and the shard count to 256.
  - Without a state directory, the node is a throwaway cluster of one: a
    fresh key, a temporary state directory removed at exit, and the cluster
    port on loopback.
  - Without `genesis` or bootstrap peers, the node founds alone.
  - Its acceptors vote at once when its state is new, or when it is alone.
    A restarted member of a larger cluster waits for catch-up (step 6), and
    warns.
  - The io runs on two threads, and the node is stopped and destroyed while
    they still run.
  - The "listening" event now carries the node's id and cluster port, and
    "founded" is reported.
- Tests: ClusterSpace, one real node and three on loopback: they found, and
  agree from any node.
- Gate on node-d (-j1): release 328 passed; differential 33 of 33
  through the new node, which founded and stopped cleanly, leaving no
  temporary state. The registrar service was untouched. Hosted CI green at
  step 1 (`742bfe6`).

## 2026-11-17: Stage 3, step 1: the register and the acceptor store

- `mimir/cluster/register.{h,cpp}`:
  - a register's name is the database (two bytes) then the key;
  - a key's shard is its Valkey slot mapped onto the shard count in
    contiguous ranges, so a hash tag shares a shard;
  - the record (the DUMP payload, absolute expiry, and recent applied
    operations with their replies) is encoded with Dannan's codec and
    decoded as untrusted input;
  - an applied operation ages out at 120 s past a later one's issue time,
    and a write is refused rather than evict a live id (at most 1024);
  - the summary is SHA-256.
- `mimir/cluster/store.{h,cpp}`: Store, Dannan's AcceptorStore in memory.
  - Every completion is posted. load() can leave values out, and keep() is
    all or nothing.
  - Deltas are asked for whole until step 7, and value() answers by exact
    ballot.
  - Not ready until caught up, unless built ready.
- Tests: ClusterRegister (9) and ClusterStore (7, against real Dannan
  Agreements wired in process).
- Gate on node-d (-j1): release 326 passed (310 plus 16). No command
  path changed, so the differential was not rerun.

## 2026-11-17: Dannan 0.9.0

- Pin moved from 0.8.5 to 0.9.0: agreement (CASPaxos per slot,
  reconfiguration, fast path, value-less prepare, deltas, change_deltas),
  the simulation harness and its checkers, and the hybrid clock. Mimir
  compiled against it unchanged, since nothing of Mimir uses agreement yet.
- Gate on node-d (-j1, shared with Lugh): release 310 passed;
  differential 33 of 33, its node stopped by PID, node-d's service
  untouched.
- Reviews given to Dannan before the tag (recorded in ACTIVE.md):
  - value-less prepare and `past`;
  - deltas in keep();
  - the final AcceptorStore;
  - change_deltas (decline, summaries);
  - the applied-twice and half-taken findings (Mimir's changes are
    repeatable by operation id; multi-name changes go through item 7);
  - the simulation seam and Wipe::store;
  - the clock's ceiling without custody.

## 2026-11-16: Dannan 0.8.5

- Pin moved from 0.8.4 to 0.8.5 (ABI kept; a shared sign-in back-off, and a
  fix for a node destroyed after its io stopped; nothing Mimir uses).
- Gate on node-d: release 310 passed on Dannan 0.8.5; differential
  33 of 33, its node stopped by PID, node-d's service untouched.

## 2026-11-16: Dannan 0.8.4

- Pin moved from 0.8.3 to 0.8.4 (Dannan: 0.8.0's ABI kept; StartTLS without
  an answer, a stoppable change wait, a deferred-answer fix; nothing Mimir
  needs). The tag reached the build servers' Dannan checkouts by bundle (no GitHub
  credentials there), working trees untouched.
- Gate on node-d (`-j1`, beside Lir's build): release 310 passed,
  built on Dannan 0.8.4 (`_deps/dannan-src` describes 0.8.4); differential
  33 of 33.

## 2026-11-16: hosted CI back, and its two findings

- The repository public, GitHub runs the jobs again (they had stopped on a
  billing hold). The first runs found two faults the build servers' GCC gate cannot:
  clang's thread-safety analysis, a tracking lambda reading `_current`
  without the lock in view; ASan, the ACL test fixture welcoming a session
  then returning it by copy, so the database held a dead frame's address
  (production unaffected: a connection's session never moves). Fixed in
  `eb17618`; hosted CI green on all seven jobs there.

## 2026-11-16: MIGRATE (ruling 31)

- As Valkey 8.1.1 answers it (probed between two Valkeys): one key or KEYS,
  COPY, REPLACE, AUTH and AUTH2, NOKEY, the target's refusal relayed ("ERR
  Target instance replied with error: ..."), IOERR for a target that cannot
  be reached or does not answer in time, and Valkey's argument errors.
- Off the database's lock, unlike Valkey's, which stops every client while it
  talks to the target: the keys are read (DUMP and remaining life) under the
  lock, the connection parks with its pipeline behind it while it talks to
  the target through Dannan's StreamConnector (AUTH, SELECT, RESTORE per
  key, one write, replies read with a bounded reader), and the database then
  removes what the target took. A key changed meanwhile is kept and the reply
  says so, which Valkey, stopping the world, cannot meet. In a transaction or
  a script it is refused (it would have to hold every client).
- `mimir/resp/reply_reader`: a bounded reader of another server's replies,
  with its fuzz target (`resp_reply_fuzz`).
- Server takes the io context for its outbound connections.
- Checked live both ways with Valkey 8.1.1: Mimir to Valkey and Valkey to
  Mimir, lists, hashes, sets, sorted sets and a life, each restored whole.
- Gate on node-a: release 310 passed (3 new); differential 33 of 33;
  `resp_reply_fuzz` (clang 19, fuzzer and undefined: ASan cannot start on
  node-a's 39-bit address space) 12.5 million runs in 60 s, nothing found.

## 2026-11-16: ruling 31, Valkey's persistence and replication commands

- SHUTDOWN stops the node gracefully (its options parsed as Valkey's; no
  reply, the connection closed; ABORT finds no shutdown in progress); SAVE,
  BGSAVE and BGREWRITEAOF refused, persistence not available until Dagda;
  REPLICAOF, SLAVEOF, SYNC, PSYNC, REPLCONF and FAILOVER refused, Mimir
  replicating through its own cluster (REPLICAOF NO ONE answers OK);
  RESTORE-ASKING is RESTORE. Commands Valkey marks no_multi are refused
  inside MULTI. MIGRATE is owed.
- mimir-node: `notify.keyspace_events` (Valkey's letters) at start, for the
  registrar's readers; `tools/deploy/` holds the service unit and the
  registrar's configuration (users stay in secrets.yaml, never here).
- node-d hung (another session's sanitizer build); the gate ran on
  node-a, whose build tree is now `/root/mimir-work/mimir/build-ci`.
  The ACL table regenerated there from mimir-node's own COMMAND LIST: eleven
  rows added, the rest unchanged.
- Gate on node-a: release 307 passed (2 new); differential 33 of 33.

## 2026-11-16: client-side caching (CLIENT TRACKING)

- As Valkey 8 documents it and 8.1.1 answers it: a read-only command's keys
  (by Valkey's key specs) are remembered for a tracking connection, and a
  change to one (a write naming it, expiry, a flush) tells it once, then
  forgets; its own changes after its reply. BCAST by prefix (one message a
  prefix, prefixes in order), OPTIN and OPTOUT with CLIENT CACHING (for the
  next command, through a transaction), NOLOOP, REDIRECT (RESP2 through a
  subscribed connection as `__redis__:invalidate` messages), a broken
  redirection told and flagged; TRACKINGINFO, GETREDIR, and every refusal in
  Valkey's words. Transactions' and scripts' reads and writes count for their
  caller; RESET ends tracking.
- Mimir invalidates on any write command naming a key, where Valkey does on a
  real change: a write that changes nothing (SET NX refused) tells readers
  too. Harmless to a cache, and stated.
- The raw-byte probe of Valkey (36 exchanges) run against mimir-node: equal
  but for the server's name and client ids.
- Gate on node-d: release 305 passed (7 new); differential 33 of 33.

## 2026-11-16: CLIENT PAUSE and UNPAUSE

- As Valkey 8.1.1 behaves (probed by timing each command under a pause): WRITE
  holds what writes or may replicate (SET, PUBLISH, EVAL, PFCOUNT, an EXEC
  whose queue writes), ALL holds everything, UNPAUSE included; a later pause
  keeps the later end and the stricter mode; active expiry stops meanwhile.
- A held command parks unrun (a wait with no reply, not a blocked command, so
  CLIENT UNBLOCK does not end it); when the pause ends or is lifted, its
  connection runs it again.
- The same timing probe run against mimir-node matched Valkey's on every line.
- The differential's RESTORE case compared an exact PTTL across the two
  servers, which raced the clock under load (one failure, 5000 against 4999);
  it compares TTL now.
- Gate on node-d: release 298 passed (2 new); differential 33 of 33.

## 2026-11-15: the rest of Valkey's command list, as a standalone Valkey answers it

- Valkey 8.1.1's COMMAND LIST (379 commands and subcommands) checked against
  mimir-node: what it called unknown is now answered.
- HELP for every container, Valkey's text (`mimir/command/help.cpp`).
  CLUSTER and ASKING as a standalone Valkey refuses them; READONLY and
  READWRITE; DEBUG, MODULE LOAD and UNLOAD refused as by default; MODULE LIST;
  PFSELFTEST; LATENCY and COMMANDLOG (8.1) with nothing sampled; MEMORY PURGE
  and MALLOC-STATS; SCRIPT SHOW and DEBUG NO; LOLWUT, Mimir's own.
- COMMAND GETKEYS and GETKEYSANDFLAGS from the key-spec table, regenerated to
  carry Valkey's full flags. COMMAND DOCS answers no documentation: Valkey's
  documentation is its text, not copied.
- CLIENT REPLY ON/OFF/SKIP, CLIENT UNBLOCK (TIMEOUT or ERROR), CAPA, GETREDIR
  and TRACKINGINFO (tracking off), IMPORT-SOURCE, and USER on LIST and KILL.
- MONITOR: every command run is pushed in Valkey's form, scripts' as "lua";
  administrative and skip_monitor commands are not, and AUTH, HELLO's AUTH
  and ACL SETUSER's rules are shown "(redacted)".
- Still owed: CLIENT PAUSE and UNPAUSE, CLIENT TRACKING and CACHING (client-
  side caching), MEMORY STATS, PFDEBUG; Valkey's persistence and replication
  commands wait on the maintainer (ACTIVE.md).
- Gate on node-d: release 296 passed (11 new); differential 33 of 33
  (the new `Differential.Introspection`), passed on its first run.

## 2026-11-15: HSCAN, SSCAN and ZSCAN page large collections (ruling 30)

- `mimir/keyspace/scan_order.h`: every hash field, set member and sorted-set
  member is numbered as it is added; a cursor is the number to go on from, so
  an element present for the whole scan is returned exactly once (Valkey
  promises at least once). The number-to-element index is built the first
  time a collection is paged and kept from then on.
- One page, cursor 0, for what Valkey keeps compact (a hash or sorted set of
  at most 128 entries, an intset or listpack set), so the differential's
  replies are unchanged; larger ones a page at a time, COUNT (default 10) a
  bound on each.
- Gate on node-d: release 285 passed (8 new); differential 32 of 32.

## 2026-11-15: README after Lir's; rulings 29 and 30; the remote public

- README rewritten in Lir's shape (why Mimir exists, what it does, the stack,
  how to use it, layout, status). The maintainer made the remote public.
- Ruling 29: MSET and DEL over several slots commit through the atomic commit
  across slots. Ruling 30: SCAN paging, above. The cross-slot proposal sent to
  Dannan and Kvasir, Kvasir's review taken, noted in Dannan's plan (item 7).

## 2026-11-15: ACL and AUTH (rulings 12, 27, 28)

- `mimir/command/acl`: Valkey's users, passwords as SHA-256 (Dannan's), the
  root selector and those in parentheses, every SETUSER rule with Valkey's
  merging (a key pattern's flags join in place; a later command or category
  rule replaces earlier ones for it and its subcommands), ACL LIST's and
  GETUSER's descriptions, and Valkey's refusal words. All or none per SETUSER.
- Keys are checked by Valkey's own key specifications, taken from Valkey
  8.1.1's COMMAND INFO into `acl_table.inc` with each command's categories
  (also COMMAND INFO's categories now); SET, BITFIELD, SORT and SORT_RO by
  Valkey's getkeys procedures. The plan's "readonly R, write W" was wrong
  (probe: COPY needs R on its source, W on its destination; SET without GET
  only W; INCR both).
- Checks in Valkey's order: unknown and arity, NOAUTH, a transaction's own
  refusals, then ACL (command, keys, channels; the most relevant refusal over
  selectors), before queueing in MULTI; EXEC checks each again; each
  `redis.call` is checked as the caller ("ERR ACL failure in script: ...").
  ACL LOG groups as Valkey's does (ten newest, a minute).
- AUTH and HELLO's AUTH; ruling 28: refused on a plaintext connection unless
  `auth.plaintext`. RESET returns to the default user. DELUSER closes the
  user's connections; SETUSER closes subscribers whose channels it revokes.
  CLIENT LIST's `user=` and `cmd=` (the full name, "client|list", as Valkey).
- Ruling 27 at one node: users seeded from `acl.users` (ACL-file lines, or
  `--user`). The parser's unauthenticated bounds are now applied.
- The differential's first run found one difference (EXEC's words for a
  command whose permissions changed after queueing), fixed.
- Gate on node-d: release 277 passed (20 new); differential 32 of 32
  (the new `Differential.Acl`, mimir-node run with `--auth-plaintext`).

## 2026-11-15: session handoff

- Landed this session (each entry below has its gate): scripting with
  Valkey's libraries and sandbox, FUNCTION, keyspace notifications, DUMP and
  RESTORE, a fuzz-found parser defect, hosted CI's leak and clang findings,
  SORT, GEO, LCS. Rulings 24 to 28 recorded.
- Final gate: release 257 passed; differential 31 of 31; hosted CI green on
  all seven jobs at `d4f18e9`. `develop` at `d0766f5`, pushed.
- Stopped before ACL; its preparation is in ACTIVE.md. No claims held.

## 2026-11-15: LCS

- LCS with LEN, IDX, MINMATCHLEN and WITHMATCHLEN, the table walked back
  from the end as Valkey walks it, so IDX's ranges and their order match;
  the transient table bounded by proto-max-bulk-len as Valkey bounds it.
- Gate on node-d: release 257 passed; differential 31 of 31.

## 2026-11-15: the GEO commands

- GEOADD (ZADD over geohashes), GEODIST, GEOHASH, GEOPOS, GEORADIUS and
  GEORADIUSBYMEMBER with their _RO forms, GEOSEARCH and GEOSEARCHSTORE.
  `mimir/keyspace/geo`: Valkey's geohash (52-bit interleave over the Web
  Mercator latitude range), its sphere and haversine, GEOHASH's re-encoding
  over the standard range, and its search: the step estimated from the
  radius, the nine cells in Valkey's order with the useless ones left out,
  each read in score order, so an unsorted answer and COUNT ANY match too.
- The gate: Valkey's documented examples as unit tests; a differential of
  about seventy cases (positions, geohashes and distances near the poles and
  the date line, unsorted, sorted and counted searches by radius and box,
  STORE and STOREDIST, and the errors in Valkey's order) passed on its first
  run.
- Gate on node-d: release 256 passed; differential 30 of 30.

## 2026-11-15: SORT and SORT_RO

- Numbers or ALPHA, DESC, LIMIT, BY (a key's value or "->" a hash field; no
  '*' sorts nothing), GET ("#" the element), STORE into a list with the
  "sortstore" notification. A set left unsorted is sorted anyway into STORE
  or in a script (Valkey's rule for determinism; scripts' sessions say so).
  Ties broken as Valkey breaks them; a stable sort where Valkey's is not.
- The differential gate's findings, matched: DESC reverses an unsorted list
  whole. Found: Valkey's ALPHA collates by its environment's locale (a UTF-8
  locale ignores '-'), so its order differs between deployments; Mimir's is
  byte order, Valkey's under LC_ALL=C, which the gate now runs it under.
- Gate on node-d: release 252 passed; differential 29 of 29.

## 2026-11-15: hosted CI's findings: a connection leak and a clang error

- Hosted CI had failed its clang and asan jobs since the server commands step
  (missed: superseded runs were cancelled and the completed ones not read).
  clang: Database::random()'s thread-safety (fixed in 27669da). asan:
  LeakSanitizer found that a connection and its stream hold each other until
  the stream finishes, so one still open when the executor stops is never
  freed. The server now tracks its open streams and `close_all()` closes them;
  the test bed and mimir-node's shutdown close all and let them finish (two
  seconds at most in the node) before stopping. A test holds that close_all
  finishes every connection.
- Gate on node-d: release 248 passed; differential 28 of 28. Hosted CI
  to confirm the asan and clang jobs.

## 2026-11-15: a parser defect the fuzz target found; fuzzing on the build servers

- The RESP request parser's fuzz target, run on node-a for the first
  time, found that an inline command was not held to `max_arguments` (a line
  of control bytes split into more words than the limit), though arrays
  were. Shown by a failing test first, then fixed: an inline command over the
  count is refused with the array's words. Bounded in production by the
  64 KB inline line, but a broken contract. The crash input passes now, and
  12 million fresh inputs ran clean; the RDB reader's target ran 9 million.
- AddressSanitizer cannot start on the build servers' 39-bit address space, so the fuzz
  targets' sanitizers are a setting (`MIMIR_FUZZ_SANITIZERS`, default with
  ASan; "fuzzer,undefined" on the build servers). clang and libFuzzer are installed on
  node-a (note posted). Two clang-only warnings fixed: Database::
  random()'s thread-safety and an unused constant.
- Gate on node-a: release 247 passed; differential 28 of 28.

## 2026-11-15: DUMP and RESTORE (and FUNCTION DUMP and RESTORE)

- `mimir/keyspace/rdb`: Valkey's RDB encoding of one value, version 11, with
  CRC-64 (Jones) and LZF written here (no dependency). Mimir writes each
  value in the encoding Valkey would hold it in, byte for byte where Valkey's
  is deterministic: integers, LZF strings, listpacks, intsets, sorted sets'
  skiplists, quicklist nodes of 8 KB, streams' node listpacks with groups,
  pending entries and consumers. It reads every encoding Valkey 8 reads, the
  ziplist ones of older payloads included; what it reads is untrusted, every
  length checked, and anything that does not parse refused ("Bad data
  format"). A libFuzzer target (`rdb_restore_fuzz`) holds that a value read
  dumps and reads back as itself.
- RESTORE with REPLACE, ABSTTL, IDLETIME and FREQ, its errors in Valkey's
  order, and the "restore" notification. FUNCTION DUMP, and FUNCTION RESTORE
  with APPEND, REPLACE and FLUSH, loaded first into a fresh interpreter so a
  payload that does not load or conflicts changes nothing.
- The gate: Valkey 8.1.1's own payloads as the unit tests' expected bytes
  (every one matched); DUMP compared byte for byte for deterministic values;
  hash tables, skiplists, quicklists and a stream with a group each restored
  from one server into the other and compared by reads both ways.
- Gate on node-a: release 246 passed; differential 28 of 28.

## 2026-11-15: keyspace notifications

- notify-keyspace-events with Valkey's classes, its error for a bad one and
  its order for CONFIG GET ('A', then K, E, m); events published on
  __keyspace@<db>__ and __keyevent@<db>__ once each command ends, inside
  EXEC and scripts too, and after active expiry.
- Every write command announces what Valkey announces, in Valkey's order (the
  push before the pop for LMOVE, an emptied key's "del" last, MOVE and COPY
  ... DB in their target database). The keyspace tells of every expired key it
  removes, however found, and of every key that comes to be ("new"); a read
  looks its keys up first, so a missing one is a "keymiss" and an expired one
  is announced, as Valkey's lookups do. SWAPDB rebinds each keyspace to its
  new number.
- The gate is Valkey's own answers: a subscriber on each server, every
  command's events compared one command at a time (about 230 commands over
  four tests). Passed on its first full run.
- Corrected a test written from Mimir's old behaviour: CONFIG GET of
  notify-keyspace-events answers Valkey's normalised form, not what was set.
- Gate on node-a: release 234 passed; differential 24 of 24.

## 2026-11-15: functions (FUNCTION, FCALL, FCALL_RO)

- A second interpreter for libraries, as Valkey keeps one, so SCRIPT FLUSH
  and FUNCTION FLUSH leave each other alone. A library loads on Valkey's
  load-time sandbox (only register_function, log, its levels and the version;
  even `type` and `error` absent), then its globals become the runtime ones,
  so its helpers see what its functions see. Loading is atomic: a refusal at
  any point changes nothing. A kept `register_function` called at run time is
  refused, never touching a load that has ended.
- FUNCTION LOAD [REPLACE], DELETE, FLUSH, LIST [LIBRARYNAME] [WITHCODE],
  STATS, KILL; FCALL and FCALL_RO with the no-writes flag; errors in
  Valkey 8's words, naming the function and `@user_function`.
- `redis.REDIS_VERSION`, `REDIS_VERSION_NUM`, `VALKEY_VERSION`,
  `VALKEY_VERSION_NUM` and `SERVER_NAME` (Mimir's own, as HELLO says) for
  scripts and functions.
- The differential gate's findings, matched: the shebang's errors and their
  order (values, name given, engine, name's characters, existing library).
  Not compared: FUNCTION LIST over several functions (Valkey answers in its
  hash order). FUNCTION DUMP and RESTORE wait for the DUMP and RESTORE step.
- Gate on node-a: release 227 passed; differential 19 of 19.

## 2026-11-15: scripting's libraries and sandbox (ruling 25)

- `cjson`, `bit` and `cmsgpack` from Debian's packages (pkg-config for the
  first two; cmsgpack ships no -dev, linked by name); `struct` written here
  to the behaviour of the struct 0.2 Valkey bundles.
- Valkey's read-only sandbox on Debian's Lua: every library and the globals
  are read-only proxies; `rawget`, `rawset`, `setmetatable`, `next`, `pairs`,
  `ipairs`, `loadstring` and `load` see through or refuse them, and never hand
  a script the table beneath; `getmetatable` answers a read-only view. Removed
  as Valkey removes them: `getfenv`, `setfenv`, `newproxy`, `module`,
  `require`, beside the files.
- The differential gate's findings, matched: "option 's' has no fixed size";
  `getmetatable(_G)` a table. Known difference: `#` of a library table is 0.
- Valkey 8.1.1 and the Lua packages installed on node-a (note posted),
  so either node-a or node-d serves the differential gate.
- Gate on node-a: release 217 passed; differential 18 of 18.

## 2026-11-15: scripting (EVAL, EVALSHA, their read-only forms, SCRIPT)

- Debian's Lua 5.1 by pkg-config (ruling 20), one interpreter for the node
  under the database's lock: a script runs whole. `server.call` and
  `redis.call` run through the transaction path (`run_locked`) with the
  script's own session, so SELECT in a script is the script's; replies
  convert by Valkey's table both ways; globals read-only; no files; errors
  carry the script's sha and line in Valkey 8's words. No Lua error crosses a
  live C++ object (errors are pushed, then raised after the scope closes).
- Not yet: `cjson`, `cmsgpack`, `bit`, `struct` (asked of the maintainer);
  `redis.setresp(3)` is accepted but replies stay RESP2; FUNCTION and FCALL
  next.
- The differential gate's finding, matched: "Wrong number of args calling
  command from script".
- Gate on node-d: release 213 passed; differential 16 of 16.
- Recorded: ruling 24 (the levels' names, `ack_level`, the group standard
  relayed by Kvasir), and Dannan's finding that a retried agreement command
  must recognise its own write.

## 2026-11-15: step 2b, second part (blocking pops, bitmaps, HyperLogLog, pub/sub, transactions, the stream log)

- Blocking pops (`e0ce2b1`): a command finding nothing is parked on its keys;
  a write wakes only the keys it named (as Valkey signals them), oldest wait
  first; a parked connection holds its pipeline and keeps reading, so a client
  that leaves takes nothing.
- Bitmaps (`63b25fe`), HyperLogLog in the shared HYLL format (`2aa59b0`:
  every estimate and a dense value's bytes equal Valkey's), pub/sub with
  RESP2's subscribed mode and RESP3 pushes (`977360e`), transactions with
  WATCH (`204bca6`), the stream log (XADD, XRANGE, XREVRANGE, XLEN, XDEL,
  XTRIM exact and by whole nodes, XREAD with BLOCK, XSETID).
- The differential gate's findings, each matched: an infinite timeout's words;
  BITCOUNT with a start alone; MULTI and WATCH refused inside a transaction
  with Valkey 8's words and aborting the EXEC; XRANGE COUNT 0 a null array;
  "(-" and "(+" invalid ids; an approximate trim may take the newest node.
- The maintainer's ruling 19 recorded: whatever can use agreement waits for Dannan's.

Gates, node-d as before: `tools/ci.sh release` **183 passed** at the
stream log, 0 warnings; the differential gate against Valkey 8.1.1 **13 of 13**
cases (strings, keys, lists, hashes, sets, sorted sets, blocking pops,
bitmaps, HyperLogLog, streams, transactions, pub/sub, connection).

## 2026-11-15: step 2b, first part (lists, hashes, sets, sorted sets)

- Lists that do not block (`bcf584a`), hashes in first-set order (`b7ebeb4`),
  sets in Valkey's encodings' orders (`d6fc37b`), sorted sets on an
  order-statistic treap (`9c76170`), each command with its class.
- The differential gate's findings, each fixed: a set leaving its integer
  shape keeps its sorted order; ZRANDMEMBER over a whole small set answers
  highest first. SCAN's order is the server's own and is no longer compared.
- Hosted CI: the Dannan repository setting was a CMake PATH, which made the
  GitHub URL a local path and failed every job (`ae3c874`, fixed; Lir told,
  since Lir's CMake has the same line).

Gates, node-d (build server 5, GCC 14.2, Release, warnings as errors, `-j2`,
`nice 19`): `tools/ci.sh release` **133 passed**, 7 differential cases skipped
there without their ports, 0 warnings; the differential gate against Valkey
8.1.1 **7 of 7 passed** (strings, keys, lists, hashes, sets, sorted sets,
connection).

## 2026-11-15: the skeleton and step 2a (one node: RESP, keyspace, strings, keys, connection)

- The project, by the 2026 style document's checklist: rules (`CLAUDE.md`,
  `CONTRIBUTING.md`), the design with the maintainer's rulings 1 to 18
  (`TODO/2026-11-15-mimir-design.md`, reviewed by the Dannan, Dagda, Lir and
  Lugh sessions), the plan, CMake on Dannan 0.8.2, `tools/ci.sh` and the hosted
  CI set, the laws (`docs/principles-and-laws.md`, verbatim).
- `resp`: the request parser (arrays of bulk strings and inline commands with
  Valkey's quoting; every length bounded before anything is allocated;
  Valkey's protocol errors word for word; the restricted limits for an
  unauthenticated connection) with a fuzz target, and the reply writer in RESP2
  and RESP3.
- `keyspace`: hash slots (CRC16 XMODEM, hash tags), glob matching (linear per
  star), keys ordered by slot then hash so a SCAN cursor is a position, expiry
  as absolute Unix ms (lazy, and a bounded active step).
- `command`: the table with each command's Valkey arity, flags and key
  positions and its class under the rulings (agreement, merge, configurable);
  strings (GET, SET with every option, SETNX, SETEX, PSETEX, GETSET, GETDEL,
  GETEX, MGET, MSET, MSETNX, INCR family, INCRBYFLOAT, APPEND, STRLEN,
  GETRANGE, SUBSTR, SETRANGE); keys (DEL, UNLINK, EXISTS, TYPE, the EXPIRE
  family with NX/XX/GT/LT, TTL family, PERSIST, RENAME, RENAMENX, COPY, KEYS,
  SCAN, DBSIZE, RANDOMKEY, TOUCH, FLUSHALL, FLUSHDB); connection (PING, ECHO,
  QUIT, SELECT 0, HELLO 2/3, AUTH's refusal with no password set, CLIENT ID,
  SETNAME, GETNAME, SETINFO, COMMAND, COUNT, INFO, LIST, INFO, TIME, RESET).
- `server` and `mimir-node`: RESP over `dannan::listen` (plain and TLS),
  pipelined replies in one write, a protocol error answered then closed, QUIT,
  YAML settings through Dannan's reader, active expiry ten times a second.
- The differential gate (`Differential.*`): the same commands to mimir-node and
  a Valkey 8.1.1 server, replies compared byte for byte. Its first run found
  two differences: COMMAND INFO's answer for an unknown name (a null bulk
  string; fixed), and COPY ... DB 1 (Mimir has one database, as Valkey's
  cluster mode does; standalone Valkey has sixteen; left out and put to the maintainer).

Gates, node-d (build server 5, Debian 13, GCC 14.2, Release, warnings as errors,
`-j2`, `nice 19`): `tools/ci.sh release` **89 passed, 3 skipped** (the
differential cases, skipped without their ports), 0 warnings; the differential
gate against Valkey 8.1.1 on loopback **3 of 3 passed**.
