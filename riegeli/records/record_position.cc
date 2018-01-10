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

#include "riegeli/records/record_position.h"

#include <stdint.h>
#include <cstring>
#include <string>

#include "riegeli/base/base.h"
#include "riegeli/base/endian.h"
#include "riegeli/base/string_view.h"

namespace riegeli {

std::string RecordPosition::Serialize() const {
  const uint64_t words[2] = {WriteBigEndian64(chunk_begin_),
                             WriteBigEndian64(record_index_)};
  return std::string(reinterpret_cast<const char*>(words), sizeof(words));
}

bool RecordPosition::Parse(string_view serialized) {
  uint64_t words[2];
  if (RIEGELI_UNLIKELY(serialized.size() != sizeof(words))) return false;
  std::memcpy(words, serialized.data(), sizeof(words));
  chunk_begin_ = ReadBigEndian64(words[0]);
  record_index_ = ReadBigEndian64(words[1]);
  return true;
}

}  // namespace riegeli