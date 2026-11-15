//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MULTI, EXEC, DISCARD, WATCH and UNWATCH, as Valkey 8 documents them
// ("Transactions"). EXEC runs its queue whole under the database's one hold
// of its lock, so no other client's command falls between two of them. In the
// cluster stage a transaction within one slot commits by one agreement round
// (design, "The two classes of command"); across slots it is CROSSSLOT.

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command::transactions {

namespace {
void end_transaction(Call& call) {
  call.session.in_multi = false;
  call.session.multi_failed = false;
  call.session.queued.clear();
}
}  // namespace

// Called under the database's lock, as every handler is.
void multi(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (call.session.in_multi) throw CommandError("ERR MULTI calls can not be nested");
  call.session.in_multi = true;
  call.session.multi_failed = false;
  call.session.queued.clear();
  call.reply.simple("OK");
}

void discard(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (!call.session.in_multi) throw CommandError("ERR DISCARD without MULTI");
  end_transaction(call);
  call.database.unwatch_locked(call.session);
  call.reply.simple("OK");
}

void exec(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (!call.session.in_multi) throw CommandError("ERR EXEC without MULTI");
  const bool failed = call.session.multi_failed;
  std::vector<std::vector<std::string>> queued = std::move(call.session.queued);
  end_transaction(call);
  if (failed) {
    call.database.unwatch_locked(call.session);
    throw CommandError("EXECABORT Transaction discarded because of previous errors.");
  }
  const bool broken = call.database.watch_broken_locked(call.session);
  // The watches end before the queue runs, so its own writes break nothing.
  call.database.unwatch_locked(call.session);
  if (broken) {
    call.reply.null_array();
    return;
  }
  call.reply.array(queued.size());
  for (const auto& args : queued) {
    // Permissions are checked again as each runs: they may have changed since
    // it was queued, as Valkey checks them, in Valkey's words.
    Denial denial = Denial::none;
    if (call.database.acl_refusal_locked(call.session, args, "multi", &denial)) {
      const char* reason = denial == Denial::command ? "no permission to execute the command or subcommand"
                           : denial == Denial::key   ? "no permission to touch the specified keys"
                                                     : "no permission to access one of the channels used as arguments";
      call.reply.error(std::string("NOPERM ACLs rules changed between the moment the transaction was accumulated and the EXEC call. This command is no longer "
                                   "allowed for the following reason: ") +
                       reason);
      continue;
    }
    call.database.run_locked(call.session, args, call.reply);
  }
}

void watch(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (call.session.in_multi) throw CommandError("ERR WATCH inside MULTI is not allowed");
  for (size_t i = 1; i < call.args.size(); ++i) call.database.watch_locked(call.session, call.args[i]);
  call.reply.simple("OK");
}

void unwatch(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  call.database.unwatch_locked(call.session);
  call.reply.simple("OK");
}

}  // namespace mimir::command::transactions
