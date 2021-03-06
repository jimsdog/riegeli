// Copyright 2018 Google LLC
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

#include "riegeli/bytes/zlib_reader.h"

#include <stddef.h>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "riegeli/base/base.h"
#include "riegeli/base/string_view.h"
#include "riegeli/bytes/buffered_reader.h"
#include "riegeli/bytes/reader.h"
#include "zlib.h"

namespace riegeli {

ZLibReader::ZLibReader() noexcept = default;

ZLibReader::ZLibReader(std::unique_ptr<Reader> src, Options options)
    : ZLibReader(src.get(), options) {
  owned_src_ = std::move(src);
}

ZLibReader::ZLibReader(Reader* src, Options options)
    : BufferedReader(options.buffer_size_), src_(RIEGELI_ASSERT_NOTNULL(src)) {
  decompressor_present_ = true;
  decompressor_.next_in = nullptr;
  decompressor_.avail_in = 0;
  decompressor_.zalloc = nullptr;
  decompressor_.zfree = nullptr;
  decompressor_.opaque = nullptr;
  if (RIEGELI_UNLIKELY(inflateInit2(&decompressor_, options.window_bits_) !=
                       Z_OK)) {
    FailOperation("inflateInit2()");
  }
}

ZLibReader::ZLibReader(ZLibReader&& src) noexcept
    : BufferedReader(std::move(src)),
      owned_src_(std::move(src.owned_src_)),
      src_(riegeli::exchange(src.src_, nullptr)),
      decompressor_present_(
          riegeli::exchange(src.decompressor_present_, false)),
      decompressor_(src.decompressor_) {}

ZLibReader& ZLibReader::operator=(ZLibReader&& src) noexcept {
  // Exchange decompressor_present_ early to support self-assignment.
  const bool decompressor_present =
      riegeli::exchange(src.decompressor_present_, false);
  if (decompressor_present_) {
    const int result = inflateEnd(&decompressor_);
    RIEGELI_ASSERT_EQ(result, Z_OK) << "inflateEnd() failed";
  }
  BufferedReader::operator=(std::move(src));
  owned_src_ = std::move(src.owned_src_);
  src_ = riegeli::exchange(src.src_, nullptr);
  decompressor_present_ = decompressor_present;
  decompressor_ = src.decompressor_;
  return *this;
}

ZLibReader::~ZLibReader() {
  if (decompressor_present_) {
    const int result = inflateEnd(&decompressor_);
    RIEGELI_ASSERT_EQ(result, Z_OK) << "inflateEnd() failed";
  }
}

void ZLibReader::Done() {
  if (RIEGELI_UNLIKELY(!Pull() && decompressor_present_)) {
    Fail("Truncated zlib-compressed stream");
  }
  if (owned_src_ != nullptr) {
    if (RIEGELI_LIKELY(healthy())) {
      if (RIEGELI_UNLIKELY(!owned_src_->Close())) Fail(*owned_src_);
    }
    owned_src_.reset();
  }
  src_ = nullptr;
  if (decompressor_present_) {
    decompressor_present_ = false;
    const int result = inflateEnd(&decompressor_);
    RIEGELI_ASSERT_EQ(result, Z_OK) << "inflateEnd() failed";
  }
  BufferedReader::Done();
}

inline bool ZLibReader::FailOperation(string_view operation) {
  std::string message = std::string(operation) + " failed";
  if (decompressor_.msg != nullptr) message += std::string(": ") + decompressor_.msg;
  return Fail(message);
}

bool ZLibReader::PullSlow() {
  RIEGELI_ASSERT_EQ(available(), 0u)
      << "Failed precondition of Reader::PullSlow(): "
         "data available, use Pull() instead";
  // After all data have been decompressed, skip BufferedReader::PullSlow()
  // to avoid allocating the buffer in case it was not allocated yet.
  if (RIEGELI_UNLIKELY(!decompressor_present_)) return false;
  return BufferedReader::PullSlow();
}

bool ZLibReader::ReadInternal(char* dest, size_t min_length,
                              size_t max_length) {
  RIEGELI_ASSERT_GT(min_length, 0u)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "nothing to read";
  RIEGELI_ASSERT_GE(max_length, min_length)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "max_length < min_length";
  RIEGELI_ASSERT(healthy())
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "Object unhealthy";
  if (RIEGELI_UNLIKELY(!decompressor_present_)) return false;
  decompressor_.next_out = reinterpret_cast<Bytef*>(dest);
  for (;;) {
    decompressor_.avail_out =
        UnsignedMin(max_length - PtrDistance(reinterpret_cast<Bytef*>(dest),
                                             decompressor_.next_out),
                    std::numeric_limits<uInt>::max());
    decompressor_.next_in = const_cast<z_const Bytef*>(
        reinterpret_cast<const Bytef*>(src_->cursor()));
    decompressor_.avail_in =
        UnsignedMin(src_->available(), std::numeric_limits<uInt>::max());
    int result = inflate(&decompressor_, Z_NO_FLUSH);
    src_->set_cursor(reinterpret_cast<const char*>(decompressor_.next_in));
    const size_t length_read =
        PtrDistance(dest, reinterpret_cast<char*>(decompressor_.next_out));
    switch (result) {
      case Z_OK:
        if (length_read >= min_length) {
          limit_pos_ += length_read;
          return true;
        }
        RIEGELI_ASSERT_EQ(decompressor_.avail_in, 0u)
            << "inflate() returned but there are still input data and output "
               "space";
        if (RIEGELI_UNLIKELY(!src_->Pull())) {
          limit_pos_ += length_read;
          if (RIEGELI_LIKELY(src_->HopeForMore())) return false;
          if (src_->healthy()) return Fail("Truncated zlib-compressed stream");
          return Fail(*src_);
        }
        continue;
      case Z_STREAM_END:
        decompressor_present_ = false;
        result = inflateEnd(&decompressor_);
        RIEGELI_ASSERT_EQ(result, Z_OK) << "inflateEnd() failed";
        limit_pos_ += length_read;
        return length_read >= min_length;
      default:
        FailOperation("inflate()");
        limit_pos_ += length_read;
        return length_read >= min_length;
    }
  }
}

bool ZLibReader::HopeForMoreSlow() const {
  RIEGELI_ASSERT_EQ(available(), 0u)
      << "Failed precondition of Reader::HopeForMoreSlow(): "
         "data available, use HopeForMore() instead";
  if (RIEGELI_UNLIKELY(!healthy())) return false;
  return decompressor_present_;
}

}  // namespace riegeli
