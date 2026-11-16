#include "vyuha/risk/discipline_table.h"

namespace vyuha::risk {

// NOLINTNEXTLINE(bugprone-exception-escape): the vector is reserved, and a full one is refused first
bool DisciplineTable::lock(const oms::Uuid& user, oms::PaperFlag mode, Nanos until) noexcept {
  for (Row& row : users_) {
    if (row.user == user && row.mode == mode) {
      row.until = until;
      return true;
    }
  }
  if (users_.size() >= capacity_) {
    return false;
  }
  users_.push_back(Row{.user = user, .mode = mode, .until = until});
  return true;
}

void DisciplineTable::clear(const oms::Uuid& user, oms::PaperFlag mode) noexcept {
  for (Row& row : users_) {
    if (row.user == user && row.mode == mode) {
      row = users_.back();
      users_.pop_back();
      return;
    }
  }
}

std::optional<Nanos> DisciplineTable::locked_until(const oms::Uuid& user,
                                                   oms::PaperFlag mode) const noexcept {
  for (const Row& row : users_) {
    if (row.user == user && row.mode == mode) {
      return row.until;
    }
  }
  return std::nullopt;
}

}  // namespace vyuha::risk
