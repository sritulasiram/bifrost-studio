// Persists the latest job snapshot so a restarted agent can report a crash (R-4).
#ifndef BIFROST_SERVICE_JOURNAL_HPP
#define BIFROST_SERVICE_JOURNAL_HPP

#include "bifrost/service/wire.hpp"

#include <optional>

namespace bifrost::service {

void journalPersist(const JobSnapshot& snapshot);
// Loads a crash snapshot (journal still marked running). Normal finished jobs
// are not written to disk.
std::optional<JobSnapshot> journalRecover();
void journalClear();

} // namespace bifrost::service

#endif