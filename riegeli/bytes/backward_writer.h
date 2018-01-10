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

#ifndef RIEGELI_BYTES_BACKWARD_WRITER_H_
#define RIEGELI_BYTES_BACKWARD_WRITER_H_

#include <stddef.h>
#include <cstring>
#include <string>
#include <utility>

#include "riegeli/base/assert.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/object.h"
#include "riegeli/base/string_view.h"

namespace riegeli {

// A BackwardWriter writes sequences of bytes to a destination, like Writer,
// but back to front.
//
// Sequential writing is supported, random access is not. Flush() is not
// supported.
class BackwardWriter : public Object {
 public:
  // Ensures that some space is available for writing: pushes previously written
  // data to the destination, and points cursor() and limit() to non-empty
  // space. If some space was already available, does nothing.
  //
  // Return values:
  //  * true  - success (available() > 0, healthy())
  //  * false - failure (available() == 0, !healthy())
  bool Push();

  // Space between start() (exclusive upper bound) and limit() (inclusive lower
  // bound) is available for writing data to it, with cursor() pointing to the
  // current position going downwards (past the next byte to write).
  //
  // Invariants:
  //   start() >= cursor() >= limit() (possibly all nullptr)
  //   if !healthy() then start() == cursor() == limit()
  char* start() const { return start_; }
  char* cursor() const { return cursor_; }
  char* limit() const { return limit_; }

  // Updates the value of cursor(). Call this during writing data under cursor()
  // to indicate how much was written, or to seek within the buffer.
  //
  // Precondition: start() >= cursor >= limit()
  void set_cursor(char* cursor);

  // Returns the amount of space available between cursor() and limit().
  //
  // Invariant: if !healthy() then available() == 0
  size_t available() const { return cursor_ - limit_; }

  // Prepends a fixed number of bytes from src to the buffer, pushing data to
  // the destination as needed.
  //
  // Return values:
  //  * true  - success (src.size() bytes written)
  //  * false - failure (a suffix of less than src.size() bytes written,
  //                    !healthy())
  bool Write(string_view src);
  bool Write(std::string&& src);
  bool Write(const char* src);
  bool Write(const Chain& src);
  bool Write(Chain&& src);

  // Returns the current position (increasing as data are prepended).
  //
  // This is not necessarily 0 after creating the BackwardWriter if it prepends
  // to a destination with existing contents, or if the BackwardWriter wraps
  // another writer or output stream propagating its position.
  //
  // This may decrease when the BackwardWriter becomes unhealthy (due to
  // buffering, previously written but unflushed data may be lost).
  //
  // This is always 0 when the BackwardWriter is closed.
  Position pos() const { return start_pos_ + written_to_buffer(); }

 protected:
  BackwardWriter() = default;

  // Moves the part of the object defined in this class.
  //
  // Precondition: &src != this
  BackwardWriter(BackwardWriter&& src) noexcept;
  void operator=(BackwardWriter&& src) noexcept;

  // BackwardWriter provides a partial override of Object::Done().
  // Derived classes must override it further and include a call to
  // BackwardWriter::Done().
  virtual void Done() override = 0;

  // Resets cursor_ and limit_ to start_. Marks the BackwardWriter as failed
  // with the specified message. Always returns false.
  //
  // Precondition: healthy()
  RIEGELI_ATTRIBUTE_COLD bool Fail(string_view message);

  // Implementation of the slow part of Push().
  //
  // Precondition: available() == 0
  virtual bool PushSlow() = 0;

  // Returns the amount of data written to the buffer, between start() and
  // cursor().
  size_t written_to_buffer() const { return start_ - cursor_; }

  // Implementation of the slow part of Write().
  //
  // By default WriteSlow(string_view) is implemented in terms of Push();
  // WriteSlow(string&&) and WriteSlow(const Chain&) are
  // implemented in terms of WriteSlow(string_view); and WriteSlow(Chain&&) is
  // implemented in terms of WriteSlow(const Chain&).
  //
  // Precondition for WriteSlow(string_view):
  //   src.size() > available()
  //
  // Precondition for WriteSlow(string&&), WriteSlow(const Chain&), and
  // WriteSlow(Chain&&):
  //   src.size() > UnsignedMin(available(), kMaxBytesToCopy())
  virtual bool WriteSlow(string_view src);
  virtual bool WriteSlow(std::string&& src);
  virtual bool WriteSlow(const Chain& src);
  virtual bool WriteSlow(Chain&& src);

  // Destination position corresponding to limit_.
  Position limit_pos() const { return start_pos_ + (start_ - limit_); }

  char* start_ = nullptr;
  char* cursor_ = nullptr;
  char* limit_ = nullptr;

  // Destination position corresponding to start_.
  Position start_pos_ = 0;
};

// Implementation details follow.

inline BackwardWriter::BackwardWriter(BackwardWriter&& src) noexcept
    : start_(riegeli::exchange(src.start_, nullptr)),
      cursor_(riegeli::exchange(src.cursor_, nullptr)),
      limit_(riegeli::exchange(src.limit_, nullptr)),
      start_pos_(riegeli::exchange(src.start_pos_, 0)) {}

inline void BackwardWriter::operator=(BackwardWriter&& src) noexcept {
  RIEGELI_ASSERT(&src != this);
  Object::operator=(std::move(src));
  start_ = riegeli::exchange(src.start_, nullptr);
  cursor_ = riegeli::exchange(src.cursor_, nullptr);
  limit_ = riegeli::exchange(src.limit_, nullptr);
  start_pos_ = riegeli::exchange(src.start_pos_, 0);
}

inline void BackwardWriter::Done() {
  start_ = nullptr;
  cursor_ = nullptr;
  limit_ = nullptr;
  start_pos_ = 0;
}

inline bool BackwardWriter::Push() {
  if (RIEGELI_LIKELY(available() > 0)) return true;
  return PushSlow();
}

inline void BackwardWriter::set_cursor(char* cursor) {
  RIEGELI_ASSERT(cursor <= start());
  RIEGELI_ASSERT(cursor >= limit());
  cursor_ = cursor;
}

inline bool BackwardWriter::Write(string_view src) {
  if (RIEGELI_LIKELY(src.size() <= available())) {
    if (!src.empty()) {  // memcpy(nullptr, _, 0) and
                         // memcpy(_, nullptr, 0) are undefined.
      cursor_ -= src.size();
      std::memcpy(cursor_, src.data(), src.size());
    }
    return true;
  }
  return WriteSlow(src);
}

inline bool BackwardWriter::Write(std::string&& src) {
  if (RIEGELI_LIKELY(src.size() <= available() &&
                     src.size() <= kMaxBytesToCopy())) {
    if (!src.empty()) {  // memcpy(nullptr, _, 0) is undefined.
      cursor_ -= src.size();
      std::memcpy(cursor_, src.data(), src.size());
    }
    return true;
  }
  return WriteSlow(std::move(src));
}

inline bool BackwardWriter::Write(const char* src) {
  return Write(string_view(src));
}

inline bool BackwardWriter::Write(const Chain& src) {
  if (RIEGELI_LIKELY(src.size() <= available() &&
                     src.size() <= kMaxBytesToCopy())) {
    cursor_ -= src.size();
    src.CopyTo(cursor_);
    return true;
  }
  return WriteSlow(src);
}

inline bool BackwardWriter::Write(Chain&& src) {
  if (RIEGELI_LIKELY(src.size() <= available() &&
                     src.size() <= kMaxBytesToCopy())) {
    cursor_ -= src.size();
    src.CopyTo(cursor_);
    return true;
  }
  return WriteSlow(std::move(src));
}

}  // namespace riegeli

#endif  // RIEGELI_BYTES_BACKWARD_WRITER_H_