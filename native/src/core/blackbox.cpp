#include "core/blackbox.h"

#include <algorithm>
#include <utility>

namespace htmlppt {

Blackbox::Blackbox(std::size_t capacity) : records_(capacity) {}

void Blackbox::Append(BlackboxRecord record) {
  if (records_.empty()) return;
  record.sequence = appended_ + 1;
  records_[next_] = std::move(record);
  next_ = (next_ + 1) % records_.size();
  ++appended_;
  if (appended_ > records_.size()) ++dropped_;
}

std::vector<BlackboxRecord> Blackbox::Snapshot() const {
  std::vector<BlackboxRecord> snapshot;
  if (appended_ == 0 || records_.empty()) return snapshot;
  const std::size_t count =
      std::min<std::size_t>(static_cast<std::size_t>(appended_),
                            records_.size());
  const std::size_t start =
      appended_ > records_.size() ? next_ : std::size_t{0};
  snapshot.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    snapshot.push_back(records_[(start + i) % records_.size()]);
  }
  return snapshot;
}

} // namespace htmlppt
