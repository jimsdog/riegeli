// Copyright 2017 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef RIEGELI_RECORDS_RECORD_POSITION_H_
#define RIEGELI_RECORDS_RECORD_POSITION_H_

#include <stdint.h>
#include <limits>
#include <string>

#include "riegeli/base/base.h"
#include "riegeli/base/string_view.h"

namespace riegeli {

class RecordPosition {
 public:
  // Creates a RecordPosition corresponding to the first record.
  RecordPosition() noexcept = default;

  // Creates a RecordPosition corresponding to the given record of the chunk
  // at the given file position.
  RecordPosition(uint64_t chunk_begin, uint64_t record_index)
      : chunk_begin_(chunk_begin), record_index_(record_index) {
    RIEGELI_ASSERT_LE(record_index,
                      std::numeric_limits<uint64_t>::max() - chunk_begin);
  }

  RecordPosition(const RecordPosition&) noexcept = default;
  RecordPosition& operator=(const RecordPosition&) noexcept = default;

  uint64_t chunk_begin() const { return chunk_begin_; }
  uint64_t record_index() const { return record_index_; }

  // Converts RecordPosition to an integer scaled between 0 and file size.
  // Distinct RecordPositions of a valid file have distinct numeric values.
  uint64_t numeric() const { return chunk_begin_ + record_index_; }

  // Serialized strings have the same natural order as the corresponding
  // positions.
  std::string Serialize() const;
  bool Parse(string_view serialized);

  friend bool operator==(RecordPosition a, RecordPosition b);
  friend bool operator!=(RecordPosition a, RecordPosition b);
  friend bool operator<(RecordPosition a, RecordPosition b);
  friend bool operator>(RecordPosition a, RecordPosition b);
  friend bool operator<=(RecordPosition a, RecordPosition b);
  friend bool operator>=(RecordPosition a, RecordPosition b);

 private:
  // Invariant: record_index_ <= numeric_limits<uint64_t>::max() - chunk_begin_
  uint64_t chunk_begin_ = 0;
  uint64_t record_index_ = 0;
};

// Implementation details follow.

inline bool operator==(RecordPosition a, RecordPosition b) {
  return a.chunk_begin_ == b.chunk_begin_ && a.record_index_ == b.record_index_;
}

inline bool operator!=(RecordPosition a, RecordPosition b) {
  return a.chunk_begin_ != b.chunk_begin_ || a.record_index_ != b.record_index_;
}

inline bool operator<(RecordPosition a, RecordPosition b) {
  if (a.chunk_begin_ != b.chunk_begin_) return a.chunk_begin_ < b.chunk_begin_;
  return a.record_index_ < b.record_index_;
}

inline bool operator>(RecordPosition a, RecordPosition b) {
  if (a.chunk_begin_ != b.chunk_begin_) return a.chunk_begin_ > b.chunk_begin_;
  return a.record_index_ > b.record_index_;
}

inline bool operator<=(RecordPosition a, RecordPosition b) {
  if (a.chunk_begin_ != b.chunk_begin_) return a.chunk_begin_ < b.chunk_begin_;
  return a.record_index_ <= b.record_index_;
}

inline bool operator>=(RecordPosition a, RecordPosition b) {
  if (a.chunk_begin_ != b.chunk_begin_) return a.chunk_begin_ > b.chunk_begin_;
  return a.record_index_ >= b.record_index_;
}

}  // namespace riegeli

#endif  // RIEGELI_RECORDS_RECORD_POSITION_H_
