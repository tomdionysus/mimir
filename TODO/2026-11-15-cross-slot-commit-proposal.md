# Atomic commit across slots: Mimir's proposal to Dannan (0.9 plan, item 7)

Mimir, 2026-11-15. For Dannan to design in beside `transfer()`, and for Kvasir
to review against its own needs. Status: Proposed; reviewed by Kvasir; noted
in Dannan's plan under item 7, which Dannan takes after its fast path, the
harness's next phase, the file-system interface and the hybrid clock (Tom's
order). Ruling 29 (Tom) settled the merge-class question: (a).

## Kvasir's review, taken (2026-11-15)

1. **Intents at the hosted system's granularity.** A Kvasir range is one
   register of many rows, so an intent on the whole register would serialise
   every transaction on a range. Dannan owns the record, the decide and the
   finishing; the hosted system lays, finds and resolves intents inside its own
   value (rows for Kvasir, keys for Mimir, where a key is its own register).
2. **The conflict rule is the hosted system's.** Older-wins is Mimir's.
   Kvasir resolves waits earlier, in its lock table and deadlock detection,
   and chooses at prepare which of two intents gives way.
3. **The record carries a commit stamp** from the hybrid clock, chosen at
   decide after every read it must follow, as Percolator's does. A reader
   taking a pending transaction's kept value is then safe under snapshot reads
   too.
4. **Known writes.** A SQL transaction is interactive, and by COMMIT every
   write is known. `transact()` takes either a function over the read values
   (Mimir) or known values (Kvasir: a function that ignores its inputs).

## Why Mimir needs it

Ruling 22 (Tom): multi-key commands over several slots are allowed and atomic
across slots: MGET, MSET, DEL, the STOREs, MULTI/EXEC. Dannan's reconfiguration
fix makes a change name one slot (its records are per slot). So a command
whose keys fall in several slots needs one commit over several slots' replica
sets: all its writes or none, and no reader that sees some of them and not the
rest.

Mimir's commands that need it, when their keys span slots:

- Agreement class, read then write: MULTI/EXEC, EVAL and FCALL, SMOVE, LMOVE
  and BLMOVE, RPOPLPUSH, RENAME and RENAMENX, COPY, MSETNX, SINTERSTORE and the
  other STOREs, SORT ... STORE, GEOSEARCHSTORE, ZRANGESTORE, BITOP, PFMERGE.
- Reads that must see one moment: MGET, SINTER and the other multi-key reads,
  WATCH's versions.
- Merge class, blind writes: MSET, DEL, UNLINK, TOUCH. Ruling 22 makes these
  atomic too; see "Merge-class writes" below.

## Recommendation: a transaction record in one slot, intents in the others

Two-phase commit whose coordinator state is itself an agreed register, so no
node is a coordinator and any node can finish a transaction another started.
This is the shape of Paxos Commit (Gray and Lamport) and of Percolator's
primary lock, built over Dannan's per-slot change().

```cpp
namespace dannan::agree {

// One slot's part of a transaction: its slot, its replica set as the caller
// would name it for change(), and its names.
struct Part { Slot slot; std::vector<identity::NodeId> replicas; std::vector<std::string> names; };

class Agreement {
 public:
  // Changes names in several slots together: `change` is given every part's
  // current values (in part order) and returns the new ones and an opaque
  // result, as change() does. Committed only if every part's names were
  // locked and the record then committed; the Outcome says which part refused
  // and why otherwise. Told once on `executor` within `deadline`.
  void transact(std::string space, std::vector<Part> parts, TransactFunction change,
                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                std::function<void(TransactOutcome)> done);
};

}  // namespace dannan::agree
```

A transaction has an id: a ballot-like triple (counter, node, epoch), unique
across restarts, and ordered, so an older transaction is a smaller id. The
first part's slot holds its record. The record is a register named
for the id in a reserved range of that slot. It holds pending, then either
committed (with every new value) or aborted.

**The round, uncontended: two rounds before the answer, one after.**

1. **Lock and read** (one change per part, in parallel, each on its own slot
   and replica set). Each part's change sets, on each of its names, an intent
   {transaction id, record's slot}. It also returns the current values. The
   record's slot does the same, and its change also writes the record as
   pending. The intent sits inside the register's value, so it travels with a
   `transfer()` and needs no new acceptor state.
2. **Compute**: the proposer runs `change` over all the values, once.
3. **Decide** (one change, on the record's slot): pending becomes committed,
   carrying the new values, only if the record is still pending. This one
   register change is the commit point. Answer the client here.
4. **Resolve** (one change per part, after the answer): replace each intent by
   its new value, or drop it if aborted. Then forget the record once every
   part is resolved (Dannan's `forget()`).

The fast path applies per slot: an uncontended part's lock is one round trip.
A transaction whose parts share one replica set (which placement makes common
on small clusters) can lock all its parts in one message.

**Whoever meets an intent finishes it.** Any change or read that finds an
intent on a name it touches reads the record (a strict read on the record's
slot):

- committed: it takes the new value from the record and may resolve the
  intent.
- aborted: it takes the value the intent kept.
- pending: a reader takes the value the intent kept. That is safe: the record
  was pending when it was read, so the commit point comes later, and the read
  is ordered before the transaction.
  A writer waits, within its deadline, for the record to decide. If the
  transaction is past its own deadline (by the hybrid clock), the writer may
  abort it by changing the record from pending to aborted. The record is one
  register, so a racing decide and abort are ordered, and exactly one wins.

**Conflicts between transactions: older wins.** A transaction that finds an
intent of a younger pending transaction aborts that one through its record. A
transaction that finds an intent of an older one waits for it, within its
deadline. Every transaction is eventually the oldest live one, so this never
deadlocks, and the deadline bounds the wait (law 2: no client waits without
bound on another's transaction).

**Why it holds.** The decide is a single register change, so exactly one
outcome is ever committed for a transaction. Every name's intent is committed
by agreement on its own slot before the decide, so any later change or read of
that name meets the intent and consults the record. A name cannot be changed
in between without meeting it. A committed record carries every new value, so
any node can roll the transaction forward after its proposer has gone. An
aborted one rolls back to the kept values.

## What Dannan carries, what the hosted system does

- Dannan: `transact()`; the intent and record encoding inside register values;
  resolving intents met by change(), read() and transfer() before the hosted
  system's function sees a value; the older-wins rule; resolving and
  forgetting after the answer.
- The hosted system: its function over the values (run once, on the proposer,
  as for change()), and its slots and replica sets, as for change().
- The owner's reduction (the merge contract, ask B) sees resolved values only.
  An intent is resolved before reduction.

## Merge-class writes (ruled: (a), ruling 29)

MSET and DEL across slots are merge-class writes: any replica applies them
without agreement. Ruling 22 asks for them to be atomic too. Two choices:

- (a) Cross-slot merge writes go through `transact()` as agreement-class
  writes. They are atomic, but a minority side refuses them, so the merge
  class's availability is lost for these commands only.
- (b) They stay merge writes, applied per slot with one hybrid stamp shared
  across their slots, so each slot converges to the same winner. That is not
  atomic to a reader between the slots' arrivals.

Mimir's recommendation is (a). It honours ruling 22 as written, and the cost
falls only on multi-slot merge writes. To be put to Tom with this proposal.

## For Kvasir

A range split moves names between ranges, which is the map-intent pattern in
Dannan's reconfiguration section. A transaction over several ranges is this
proposal, with Kvasir's own record placement. Kvasir: are its SQL
transactions this shape (read, compute once, write), or do they need several
read-write rounds before the decide? If several, the record stays pending
across them and the same rules hold, with the intents laid as the reads go.

## Questions for Dannan

1. The record's names: a reserved prefix within the slot (a name byte range no
   hosted name can take), or a per-space side table beside the slot record?
2. Intent resolution inside change(): acceptable that a change meeting an
   intent costs a strict read of another slot (one more round)? It is rare
   under the older-wins rule and only on contended names.
3. Deadlines across the parts: the transaction's deadline bounds the client's
   wait; the record's own expiry (by the hybrid clock, 0.9's) is when others
   may abort it. Same value, or the record's longer?
