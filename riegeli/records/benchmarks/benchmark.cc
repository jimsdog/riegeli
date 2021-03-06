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

// Make file offsets 64-bit even on 32-bit systems.
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "riegeli/base/base.h"
#include "riegeli/base/str_error.h"
#include "riegeli/bytes/fd_reader.h"
#include "riegeli/bytes/fd_writer.h"
#include "riegeli/bytes/writer_utils.h"
#include "riegeli/records/benchmarks/tfrecord_recognizer.h"
#include "riegeli/records/chunk_reader.h"
#include "riegeli/records/record_reader.h"
#include "riegeli/records/record_writer.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/io/compression.h"
#include "tensorflow/core/lib/io/record_reader.h"
#include "tensorflow/core/lib/io/record_writer.h"
#include "tensorflow/core/platform/env.h"

namespace {

uint64_t FileSize(const std::string& filename) {
  struct stat stat_info;
  const int result = stat(filename.c_str(), &stat_info);
  RIEGELI_CHECK_EQ(result, 0) << "stat() failed: " << riegeli::StrError(errno);
  return riegeli::IntCast<uint64_t>(stat_info.st_size);
}

uint64_t CpuTimeNow_ns() {
  struct timespec time_info;
  RIEGELI_CHECK_EQ(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time_info), 0);
  return riegeli::IntCast<uint64_t>(time_info.tv_sec) * uint64_t{1000000000} +
         riegeli::IntCast<uint64_t>(time_info.tv_nsec);
}

uint64_t RealTimeNow_ns() {
  struct timespec time_info;
  RIEGELI_CHECK_EQ(clock_gettime(CLOCK_MONOTONIC, &time_info), 0);
  return riegeli::IntCast<uint64_t>(time_info.tv_sec) * uint64_t{1000000000} +
         riegeli::IntCast<uint64_t>(time_info.tv_nsec);
}

class Stats {
 public:
  void Add(double value);

  double Average() const;
  double StdDev() const;

 private:
  // Based on https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
  int count_ = 0;
  double sample_ = 0.0;
  double residual_sum_ = 0.0;
  double residual_sum_of_squares_ = 0.0;
};

void Stats::Add(double value) {
  if (++count_ == 1) {
    sample_ = value;
  } else {
    const double residual = value - sample_;
    residual_sum_ += residual;
    residual_sum_of_squares_ += residual * residual;
  }
}

double Stats::Average() const {
  RIEGELI_CHECK_GT(count_, 0) << "No data";
  return sample_ + residual_sum_ / static_cast<double>(count_);
}

double Stats::StdDev() const {
  RIEGELI_CHECK_GT(count_, 0) << "No data";
  if (count_ == 1) return 0.0;
  return std::sqrt(std::max(
      0.0, (residual_sum_of_squares_ -
            residual_sum_ * residual_sum_ / static_cast<double>(count_)) /
               static_cast<double>(count_ - 1)));
}

class Benchmarks {
 public:
  static bool ReadFile(const std::string& filename, std::vector<std::string>* records,
                       size_t* max_size);

  Benchmarks(std::vector<std::string> records, std::string output_dir, int repetitions);

  void RunAll();

 private:
  static void WriteTFRecord(
      const std::string& filename,
      const tensorflow::io::RecordWriterOptions& record_writer_options,
      const std::vector<std::string>& records);
  static bool ReadTFRecord(
      const std::string& filename,
      const tensorflow::io::RecordReaderOptions& record_reader_options,
      std::vector<std::string>* records, size_t* max_size = nullptr);

  static void WriteRiegeli(const std::string& filename,
                           riegeli::RecordWriter::Options record_writer_options,
                           const std::vector<std::string>& records);
  static bool ReadRiegeli(const std::string& filename,
                          riegeli::RecordReader::Options record_reader_options,
                          std::vector<std::string>* records,
                          size_t* max_size = nullptr);

  void RunOne(
      const std::string& name,
      std::function<void(const std::string&, const std::vector<std::string>&)>
          write_records,
      std::function<void(const std::string&, std::vector<std::string>*)> read_records);

 private:
  static constexpr int MaxNameWidth() { return 30; }

  std::vector<std::string> records_;
  size_t original_size_;
  std::string output_dir_;
  int repetitions_;
};

bool Benchmarks::ReadFile(const std::string& filename, std::vector<std::string>* records,
                          size_t* max_size) {
  riegeli::FdReader file_reader(filename, O_RDONLY);
  {
    riegeli::TFRecordDetector tfrecord_recognizer(&file_reader);
    tensorflow::io::RecordReaderOptions record_reader_options;
    if (tfrecord_recognizer.CheckFileFormat(&record_reader_options)) {
      RIEGELI_CHECK(tfrecord_recognizer.Close())
          << tfrecord_recognizer.Message();
      RIEGELI_CHECK(file_reader.Close()) << file_reader.Message();
      std::cout << "Reading TFRecord: " << filename << std::endl;
      return ReadTFRecord(filename, record_reader_options, records, max_size);
    }
  }
  RIEGELI_CHECK(file_reader.Seek(0)) << file_reader.Message();
  {
    riegeli::ChunkReader chunk_reader(&file_reader);
    if (chunk_reader.CheckFileFormat()) {
      RIEGELI_CHECK(chunk_reader.Close()) << chunk_reader.Message();
      RIEGELI_CHECK(file_reader.Close()) << file_reader.Message();
      std::cout << "Reading Riegeli/records: " << filename << std::endl;
      return ReadRiegeli(filename, riegeli::RecordReader::Options(), records,
                         max_size);
    }
  }
  std::cerr << "Unknown file format: " << filename << std::endl;
  std::exit(1);
}

void Benchmarks::WriteTFRecord(
    const std::string& filename,
    const tensorflow::io::RecordWriterOptions& record_writer_options,
    const std::vector<std::string>& records) {
  tensorflow::Env* const env = tensorflow::Env::Default();
  std::unique_ptr<tensorflow::WritableFile> file_writer;
  {
    const tensorflow::Status status =
        env->NewWritableFile(filename, &file_writer);
    RIEGELI_CHECK(status.ok()) << status;
  }
  tensorflow::io::RecordWriter record_writer(file_writer.get(),
                                             record_writer_options);
  for (const auto& record : records) {
    const tensorflow::Status status = record_writer.WriteRecord(record);
    RIEGELI_CHECK(status.ok()) << status;
  }
  const tensorflow::Status status = record_writer.Close();
  RIEGELI_CHECK(status.ok()) << status;
}

bool Benchmarks::ReadTFRecord(
    const std::string& filename,
    const tensorflow::io::RecordReaderOptions& record_reader_options,
    std::vector<std::string>* records, size_t* max_size) {
  size_t max_size_storage = std::numeric_limits<size_t>::max();
  if (max_size == nullptr) max_size = &max_size_storage;
  tensorflow::Env* const env = tensorflow::Env::Default();
  std::unique_ptr<tensorflow::RandomAccessFile> file_reader;
  {
    const tensorflow::Status status =
        env->NewRandomAccessFile(filename, &file_reader);
    RIEGELI_CHECK(status.ok()) << status;
  }
  tensorflow::io::SequentialRecordReader record_reader(file_reader.get(),
                                                       record_reader_options);
  std::string record;
  for (;;) {
    const tensorflow::Status status = record_reader.ReadRecord(&record);
    if (!status.ok()) {
      RIEGELI_CHECK(tensorflow::errors::IsOutOfRange(status)) << status;
      break;
    }
    const size_t memory =
        riegeli::LengthVarint64(record.size()) + record.size();
    if (RIEGELI_UNLIKELY(*max_size < memory)) return false;
    *max_size -= memory;
    records->push_back(std::move(record));
  }
  return true;
}

void Benchmarks::WriteRiegeli(
    const std::string& filename,
    riegeli::RecordWriter::Options record_writer_options,
    const std::vector<std::string>& records) {
  riegeli::FdWriter file_writer(filename, O_WRONLY | O_CREAT | O_TRUNC);
  riegeli::RecordWriter record_writer(&file_writer, record_writer_options);
  for (const auto& record : records) {
    RIEGELI_CHECK(record_writer.WriteRecord(record)) << record_writer.Message();
  }
  RIEGELI_CHECK(record_writer.Close()) << record_writer.Message();
  RIEGELI_CHECK(file_writer.Close()) << file_writer.Message();
}

bool Benchmarks::ReadRiegeli(
    const std::string& filename,
    riegeli::RecordReader::Options record_reader_options,
    std::vector<std::string>* records, size_t* max_size) {
  size_t max_size_storage = std::numeric_limits<size_t>::max();
  if (max_size == nullptr) max_size = &max_size_storage;
  riegeli::FdReader file_reader(filename, O_RDONLY);
  riegeli::RecordReader record_reader(&file_reader, record_reader_options);
  std::string record;
  while (record_reader.ReadRecord(&record)) {
    const size_t memory =
        riegeli::LengthVarint64(record.size()) + record.size();
    if (RIEGELI_UNLIKELY(*max_size < memory)) return false;
    *max_size -= memory;
    records->push_back(std::move(record));
  }
  RIEGELI_CHECK(record_reader.Close()) << record_reader.Message();
  RIEGELI_CHECK(file_reader.Close()) << file_reader.Message();
  return true;
}

Benchmarks::Benchmarks(std::vector<std::string> records, std::string output_dir,
                       int repetitions)
    : records_(std::move(records)),
      original_size_(0),
      output_dir_(std::move(output_dir)),
      repetitions_(repetitions) {
  for (const auto& record : records_) {
    original_size_ += riegeli::LengthVarint64(record.size()) + record.size();
  }
}

void Benchmarks::RunOne(
    const std::string& name,
    std::function<void(const std::string&, const std::vector<std::string>&)>
        write_records,
    std::function<void(const std::string&, std::vector<std::string>*)> read_records) {
  const std::string filename = output_dir_ + "/record_benchmark_" + name;

  Stats compression;
  Stats writing_cpu_speed;
  Stats writing_real_speed;
  Stats reading_cpu_speed;
  Stats reading_real_speed;
  for (int i = 0; i < repetitions_ + 1; ++i) {
    const uint64_t cpu_time_before_ns = CpuTimeNow_ns();
    const uint64_t real_time_before_ns = RealTimeNow_ns();
    write_records(filename, records_);
    const uint64_t cpu_time_after_ns = CpuTimeNow_ns();
    const uint64_t real_time_after_ns = RealTimeNow_ns();
    if (i == 0) {
      // Warm-up.
    } else {
      compression.Add(static_cast<double>(FileSize(filename)) /
                      static_cast<double>(original_size_) * 100.0);
      writing_cpu_speed.Add(
          static_cast<double>(original_size_) /
          static_cast<double>(cpu_time_after_ns - cpu_time_before_ns) * 1000.0);
      writing_real_speed.Add(
          static_cast<double>(original_size_) /
          static_cast<double>(real_time_after_ns - real_time_before_ns) *
          1000.0);
    }
  }
  for (int i = 0; i < repetitions_ + 1; ++i) {
    std::vector<std::string> decoded_records;
    const uint64_t cpu_time_before_ns = CpuTimeNow_ns();
    const uint64_t real_time_before_ns = RealTimeNow_ns();
    read_records(filename, &decoded_records);
    const uint64_t cpu_time_after_ns = CpuTimeNow_ns();
    const uint64_t real_time_after_ns = RealTimeNow_ns();
    if (i == 0) {
      // Warm-up and correctness check.
      RIEGELI_CHECK(decoded_records == records_)
          << "Decoded records do not match for " << name;
    } else {
      reading_cpu_speed.Add(
          static_cast<double>(original_size_) /
          static_cast<double>(cpu_time_after_ns - cpu_time_before_ns) * 1000.0);
      reading_real_speed.Add(
          static_cast<double>(original_size_) /
          static_cast<double>(real_time_after_ns - real_time_before_ns) *
          1000.0);
    }
  }

  std::cout << std::left << std::setw(MaxNameWidth()) << name << ' '
            << std::right << std::setw(6) << std::fixed << std::setprecision(2)
            << compression.Average() << std::setprecision(0);
  for (const auto& stats : {writing_cpu_speed, writing_real_speed,
                            reading_cpu_speed, reading_real_speed}) {
    std::cout << ' ' << std::right << std::setw(4) << stats.Average() << "±"
              << std::left << std::setw(3) << stats.StdDev();
  }
  std::cout << std::endl;
}

void Benchmarks::RunAll() {
  std::cout << "Original size: " << std::fixed << std::setprecision(3)
            << (static_cast<double>(original_size_) / 1000000.0) << " MB"
            << std::endl;
  std::cout << std::left << std::setw(MaxNameWidth()) << ""
            << "  Comp   Wr CPU   Wr Real  Rd CPU   Rd Real" << std::endl;
  std::cout << std::setw(MaxNameWidth()) << "Format"
            << "    %     MB/s     MB/s     MB/s     MB/s" << std::endl;
  std::cout << std::setfill('-') << std::setw(MaxNameWidth() + 43) << ""
            << std::setfill(' ') << std::endl;

  RunOne("tfrecord_uncompressed",
         [&](const std::string& filename, const std::vector<std::string>& records) {
           WriteTFRecord(
               filename,
               tensorflow::io::RecordWriterOptions::CreateRecordWriterOptions(
                   tensorflow::io::compression::kNone),
               records);
         },
         [&](const std::string& filename, std::vector<std::string>* records) {
           ReadTFRecord(
               filename,
               tensorflow::io::RecordReaderOptions::CreateRecordReaderOptions(
                   tensorflow::io::compression::kNone),
               records);
         });
  RunOne("tfrecord_gzip",
         [&](const std::string& filename, const std::vector<std::string>& records) {
           WriteTFRecord(
               filename,
               tensorflow::io::RecordWriterOptions::CreateRecordWriterOptions(
                   tensorflow::io::compression::kGzip),
               records);
         },
         [&](const std::string& filename, std::vector<std::string>* records) {
           return ReadTFRecord(
               filename,
               tensorflow::io::RecordReaderOptions::CreateRecordReaderOptions(
                   tensorflow::io::compression::kGzip),
               records);
         });

  RunOne("riegeli_uncompressed",
         [&](const std::string& filename, const std::vector<std::string>& records) {
           WriteRiegeli(filename,
                        riegeli::RecordWriter::Options().DisableCompression(),
                        records);
         },
         [&](const std::string& filename, std::vector<std::string>* records) {
           return ReadRiegeli(filename, riegeli::RecordReader::Options(),
                              records);
         });
  RunOne("riegeli_notrans_brotli6",
         [&](const std::string& filename, const std::vector<std::string>& records) {
           WriteRiegeli(filename,
                        riegeli::RecordWriter::Options()
                            .EnableBrotliCompression(6)
                            .set_transpose(false),
                        records);
         },
         [&](const std::string& filename, std::vector<std::string>* records) {
           return ReadRiegeli(filename, riegeli::RecordReader::Options(),
                              records);
         });
  RunOne("riegeli_notrans_zstd9",
         [&](const std::string& filename, const std::vector<std::string>& records) {
           WriteRiegeli(filename,
                        riegeli::RecordWriter::Options()
                            .EnableZstdCompression(9)
                            .set_transpose(false),
                        records);
         },
         [&](const std::string& filename, std::vector<std::string>* records) {
           return ReadRiegeli(filename, riegeli::RecordReader::Options(),
                              records);
         });
  RunOne(
      "riegeli_trans_uncompressed",
      [&](const std::string& filename, const std::vector<std::string>& records) {
        WriteRiegeli(
            filename,
            riegeli::RecordWriter::Options().DisableCompression().set_transpose(
                true),
            records);
      },
      [&](const std::string& filename, std::vector<std::string>* records) {
        return ReadRiegeli(filename, riegeli::RecordReader::Options(), records);
      });
  RunOne("riegeli_trans_brotli6",
         [&](const std::string& filename, const std::vector<std::string>& records) {
           WriteRiegeli(
               filename,
               riegeli::RecordWriter::Options().EnableBrotliCompression(6),
               records);
         },
         [&](const std::string& filename, std::vector<std::string>* records) {
           return ReadRiegeli(filename, riegeli::RecordReader::Options(),
                              records);
         });
  RunOne("riegeli_trans_brotli6_par10",
         [&](const std::string& filename, const std::vector<std::string>& records) {
           WriteRiegeli(filename,
                        riegeli::RecordWriter::Options()
                            .EnableBrotliCompression(6)
                            .set_parallelism(10),
                        records);
         },
         [&](const std::string& filename, std::vector<std::string>* records) {
           return ReadRiegeli(filename, riegeli::RecordReader::Options(),
                              records);
         });
  RunOne(
      "riegeli_trans_zstd9",
      [&](const std::string& filename, const std::vector<std::string>& records) {
        WriteRiegeli(filename,
                     riegeli::RecordWriter::Options().EnableZstdCompression(9),
                     records);
      },
      [&](const std::string& filename, std::vector<std::string>* records) {
        return ReadRiegeli(filename, riegeli::RecordReader::Options(), records);
      });
}

const char kUsage[] =
    "Usage: benchmark OPTION... FILE...\n"
    "\n"
    "FILEs may be TFRecord or Riegeli/records files.\n"
    "\n"
    "OPTIONs:\n"
    "  --max_size=BYTES\n"
    "      Maximum size of records to read, in bytes, default 10000000\n"
    "  --output_dir=DIR\n"
    "      Directory to write files to (files are named record_benchmark_*), "
          "default /tmp\n"
    "  --repetitions=N\n"
    "      Number of times to repeat each benchmark, default 5";

const struct option kOptions[] = {
    {"help", no_argument, nullptr, 'h'},
    {"max_size", required_argument, nullptr, 's'},
    {"output_dir", required_argument, nullptr, 'd'},
    {"repetitions", required_argument, nullptr, 'r'},
    {nullptr, 0, nullptr, 0}};

}  // namespace

int main(int argc, char** argv) {
  size_t max_size = size_t{100} * 1000 * 1000;
  std::string output_dir = "/tmp";
  int repetitions = 5;
  for (;;) {
    int option_index;
    const int option =
        getopt_long_only(argc, argv, "", kOptions, &option_index);
    if (option == -1) break;
    switch (option) {
      case 'h':
        std::cout << kUsage << std::endl;
        return 0;
      case 's': {
        errno = 0;
        char* end;
        max_size = riegeli::IntCast<size_t>(std::strtoul(optarg, &end, 10));
        if (RIEGELI_UNLIKELY(errno != 0 || *optarg == '\0' || *end != '\0')) {
          std::cerr << argv[0]
                    << ": option '--max_size' requires an integer argument\n";
          return 1;
        }
      } break;
      case 'd':
        output_dir = std::string(optarg);
        break;
      case 'r': {
        errno = 0;
        char* end;
        repetitions = riegeli::IntCast<int>(std::strtol(optarg, &end, 10));
        if (RIEGELI_UNLIKELY(errno != 0 || *optarg == '\0' || *end != '\0')) {
          std::cerr
              << argv[0]
              << ": option '--repetitions' requires an integer argument\n";
          return 1;
        }
      } break;
      case '?':
        return 1;
      default:
        RIEGELI_ASSERT_UNREACHABLE()
            << "getopt_long_only() returned " << option;
    }
  }
  argc -= optind - 1;
  argv += optind - 1;
  std::vector<std::string> records;
  if (argc == 1) {
    std::cerr << kUsage << std::endl;
    return 1;
  }
  for (int i = 1; i < argc; ++i) {
    if (!Benchmarks::ReadFile(argv[i], &records, &max_size)) break;
  }
  Benchmarks benchmarks(std::move(records), std::move(output_dir), repetitions);
  benchmarks.RunAll();
}
