# Mimir - Completed

The record, newest first. Each entry says what landed, with its gate counts.

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
