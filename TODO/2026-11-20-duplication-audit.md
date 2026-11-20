# Mimir: audit for duplicated functionality (2026-11-20)

The maintainer, relayed to every project: "Any functionality that is required by two or
more projects that share a project dependency, that functionality goes into
the shared dependency." Mimir depends on Dannan (and on Dagda from stage 5).
What Mimir holds that another project also needs, or that a dependency
already has. Nothing here has been moved yet; Mimir never edits a sibling's
tree, so each move is a request to the owning session once the maintainer confirms.

## Belongs in Dannan: agreement plumbing every agreement user needs

Mimir built these on top of Dannan's agreement. Louhi already implements
AcceptorStore too (`louhi/admit/agreement.h`), and Kvasir and Lugh's TOTP
lease are named consumers, so each is needed by two or more projects that
share Dannan.

1. **Catch-up after a restart.** Mimir's `cluster::Agreements::recover`:
   - a user route (601 `mimir.shard_names`) asks a ready peer for the names
     it holds in a slot, paged;
   - Dannan's catch_up() then reads them at a majority;
   - the store is marked ready only after that.
   Any system whose store restarts empty needs the same. Dannan should
   offer the names-in-a-slot exchange and the recovery loop, and the store
   only answer `held(slot)`.
2. **Commit notices.** Mimir's user route 600 `mimir.commit_notice` tells
   a register's other replicas that a commit happened, with the committing
   ballot as an ordered version, so their committed views (ruling 32)
   catch up without a read. Dannan's agreement never tells a replica that
   a write committed. Any system serving reads from a local view needs
   this; it is also what Dagda's "caught up" question (Louhi's need)
   circles. Dannan should send it, best effort, as part of change().
3. **Exactly-once for repeatable changes.**
   - Mimir makes operation ids (node, durability epoch, counter).
   - It keeps the applied ids in each record with their replies, bounded
     by count and a 120 s horizon (`cluster::note_applied`), so a retried
     change answers as the first did.
   Dannan has the `repeatable` flag but leaves the id and its bookkeeping
   to each user. Every user of repeatable changes needs exactly this.
4. **A ballot as ordered bytes** (`version_of` in
   `mimir/cluster/agreements.cpp`), for comparing commits. A small one,
   but it is Dannan's type.

## Belongs in Dagda (or Dannan): the in-memory AcceptorStore

5. Mimir's `cluster::Store` is an in-memory AcceptorStore.
   - Dannan has two more in its tests (`MemoryStore`, the sim's `Store`),
     and Louhi has its own.
   - Dagda is now building the durable store for every agreement user, with
     a durability level per space (ephemeral included) and a value-less
     form (Dagda, 2026-11-20).
   - When it lands, Mimir should use it at `ephemeral` and drop its own.
     Until then Mimir's stays, since stage 3 cannot wait.

## Belongs in Dannan: SHA-1

6. Mimir computes SHA-1 by calling OpenSSL directly
   (`sha1_hex` in `mimir/command/scripting.cpp`, for script SHAs).
   - Lugh has SHA-1 for TOTP (`lugh/authn/totp.cpp`), and Macha for
     torrents (`src/torrent/torrent_disk_io.cpp`).
   - Three projects sharing Dannan, whose crypto primitives Mimir is meant
     to link, so SHA-1 (and HMAC-SHA-1, for Lugh) belongs in
     `dannan::identity`.

## Belongs in Dannan when built: interest by announcement

7. Mimir's pub/sub fan-out sends every publication to every node over one
   notify channel. Fanning out only to nodes that announced interest is
   what Lir's interest index does. The design already says that when Mimir
   builds it, it is proposed for Dannan rather than written twice. It is
   unchanged, and now covered by this rule.

## Fixed in Mimir

8. The users' record (ACL by agreement, 2026-11-20) was length-prefixed by
   hand. It now uses Dannan's codec.

## Mimir's own, needed by no one else

These were checked and have no second user in the stack:
- the RESP parser and writer;
- Valkey's RDB format (CRC-64 Jones, LZF, the type encodings);
- CRC16 hash slots;
- Valkey's glob matching (Lir's MQTT wildcards are a different grammar);
- HyperLogLog, geohash;
- the Lua scripting host;
- ACL;
- `FixedClock` (a WallClock fixed at an operation's issue time). Dannan's
  stepped clocks are test fixtures; this one is production code, and only
  Mimir needs it.

## Recommendation

Items 1 to 4 and 6 go to Dannan, and item 5 to Dagda (already in hand).
They are asked of those sessions in that order once the maintainer confirms. Mimir
keeps its own copies until each lands, then deletes them and moves its
pin.
