# Contributing to Mimir

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

## Read first

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Mimir is
the distributed database, a Redis-compatible, multi-master, leaderless
datastore, and a hosted system on a stack with one set of conventions: Dannan
the network layer (`../dannan`), Dagda the store (`../dagda`), Lugh the
directory (`../lugh`) and Lir the event system (`../lir`).
`../dagda/TODO/2026-11-03-stack.md` says what each owns. Hosted systems use
Dannan as their network layer and Dagda for what they persist, and no code is
duplicated between the projects. The design is
`TODO/2026-11-15-mimir-design.md`.

`../dannan/CONTRIBUTING.md` is the reference text for the conventions the
projects share; this file is kept identical to it in everything but the
project's name, its build line and its "Read first".

The four laws and five disciplines (`docs/principles-and-laws.md`, a verbatim
copy of Macha's) govern every change here, cited by number. A change that
touches scheduling, admission, priority, retry or recovery says which law it
serves and which discipline it follows.

## Branches

- `main` is the last stable state. Nothing is committed to it directly; stable
  states reach it by merging `develop`.
- `develop` is where work happens.
- Features and milestones are developed on branches off `develop` as
  `feature/<short-name>` and squashed onto `develop` as one commit on
  completion. Small fixes and documents may go to `develop` directly.

## Tags and releases

- Tags are exactly `x.y.z` semver: annotated, no `v` prefix, no suffix, cut from
  `main`.
- The tag, the CMake `project(... VERSION ...)` and `--version` are the same
  number.

## Commits

- Subjects are short imperative prose. No emoji, no ticket prefixes.
- Keep unrelated changes in separate commits: a move, a rename, a fix and a feature
  are four commits. A commit that moves files does not change their content.
- A commit message ends with its last line of prose. No tool attribution trailers.

## Building and testing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMIMIR_BUILD_TESTING=ON -DMIMIR_WARNINGS_AS_ERRORS=ON
cmake --build build -j2
./build/mimir_tests
```

or `tools/ci.sh release`, which does the same. Dannan is fetched at its pinned
tag from the sibling checkout, or from its repository. Run the test binary
rather than `ctest`. Sanitizers run before a release, or to hunt a bug that
cannot be pinned any other way. Hosted CI runs `tools/ci.sh`'s check, release,
clang, client, package, tidy and coverage steps on each push to `develop` or
`main` that changes more than documents, and no sanitizer step; a feature
branch's gate runs on the build servers. CodeQL runs on `main` and weekly.
`tools/ci.sh client` builds `mimir_client_tests` (libhiredis-dev), which drives
three real mimir-node processes with hiredis (ruling 44); `client-asan` runs it
with every node under ASan. `tools/ci.sh fuzz` runs each libFuzzer target for
`MIMIR_FUZZ_SECONDS` (300). `tools/ci.sh package` builds, lints, installs,
checks and purges the package, so run it in a container or a CI runner. The
sanitizer and fuzz steps build Dannan instrumented and fail unless it carries
the sanitizer's symbols. The differential gate against a Valkey 8 server runs
by hand on the build servers.

The tests need GoogleTest (`libgtest-dev`). Build options: `MIMIR_BUILD_TESTING`,
`MIMIR_BUILD_CLIENT_TESTS` (`mimir_client_tests`: hiredis against a real
three-node cluster; needs libhiredis-dev), `MIMIR_BUILD_FUZZING` (clang,
libFuzzer targets for the RESP parser and `RESTORE`), `MIMIR_WARNINGS_AS_ERRORS`,
and `MIMIR_FETCH_DANNAN` (on by default: fetch Dannan when no installed release
of its minor is found).

Write tests from Valkey 8's documented behaviour, the design and the laws, not
from the current behaviour.

### The differential gate

The `Differential.*` cases send the same commands to Mimir and to a Valkey 8
server on loopback and compare the replies byte for byte. `tools/differential.sh
[build directory]` runs each case against a freshly started node (while
multi-key writes are refused, a case cannot clear the node with `FLUSHALL`), and
prints the count of differing replies. By hand:

```
LC_ALL=C valkey-server --port 7379 --bind 127.0.0.1 --save "" --appendonly no --daemonize yes
./build/mimir-node --listen 127.0.0.1 --port 7380 --databases 16 --auth-plaintext --seconds 200 &
MIMIR_DIFF_MIMIR_PORT=7380 MIMIR_DIFF_VALKEY_PORT=7379 ./build/mimir_tests --gtest_filter='Differential.*'
```

## Dependencies

Dannan, Dagda (from the persistence stage), Boost (Asio), OpenSSL and yaml-cpp
(both through Dannan, never linked by Mimir itself), Lua 5.1 with lua-cjson, lua-cmsgpack
and lua-bitop as system packages (scripting), plus GoogleTest for the test
build and hiredis for the real-client gate (test-only, ruling 44).
Prefer writing something by hand over adding a library, and say so if a
dependency really is the right call.

## Layout

One layout for all the projects, as `../dannan/CONTRIBUTING.md` "Layout" gives
it, with `<p>` = `mimir`: the library target `mimir` in `mimir/`, public headers
in `mimir/include/mimir/`, subsystems in `mimir/<subsystem>/` (`resp`,
`keyspace`, `command`, `cluster`, `server`), tests in `mimir/test/` (one binary
`mimir_tests`, and `mimir_client_tests` from `mimir/test/client/`), fuzz
targets in `mimir/fuzz/` (the RESP request parser first), tools in `tools/`,
the Debian packaging in `debian/` with the packaged settings and unit in
`packaging/` (`docs/packaging.md`), the manual page in `docs/man/`, and the
design, the plan and the record in `TODO/`. No presets.

## Shared code

What more than one project needs lives once, in the lowest project that needs
it, and the others link it. Mimir links Dannan's listener, configuration
reader, crypto primitives, logging, time sources, thread-safety contract,
Status surface and test fixtures, and writes none of its own. When Mimir finds
itself writing something another project has, or needs, it asks that
project's session instead.

## Errors

An operation that could not happen throws (`std::invalid_argument` for a
caller's mistake, `std::runtime_error` or a named subclass for bytes or a
system that refused). An operation that happened returns its result, and one
that happened on more than one node returns its evidence (who answered, what
failed and why), never a bare boolean. A read that finds nothing succeeds with
an empty value. A command's refusal is its RESP error, in Valkey's words.

## Style

- Formatting is `.clang-format` (Google base, 160 columns): run `clang-format -i`
  on the files you changed. Two-space indent, `#pragma once`, braces on the same
  line, headers `.h`.
- `snake_case` for functions, methods, variables and files; `PascalCase` for
  types; private and protected members prefixed with `_`. Namespaces are
  `<project>::<subsystem>`.
- Every source file starts with the Mimir header block (`CLAUDE.md` has it).
- No emoji anywhere.
- Dependency injection over globals; the only process global is the io context.
- Each component logs through its own scoped logger.
- Thread-safety annotations on every guarded member; contracts declare their
  waits and thread safety beside each operation.
- Comments are sparse and explain intent, not mechanics.

## Plan

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Move items
across as they land.
