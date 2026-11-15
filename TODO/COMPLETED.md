# Mimir - Completed

The record, newest first. Each entry says what landed, with its gate counts.

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
