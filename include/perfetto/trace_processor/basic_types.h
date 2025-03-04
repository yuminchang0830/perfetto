/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_BASIC_TYPES_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_BASIC_TYPES_H_

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

#include "perfetto/base/export.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

// Various places in trace processor assume a max number of CPUs to keep code
// simpler (e.g. use arrays instead of vectors).
constexpr size_t kMaxCpus = 128;

// All metrics protos are in this directory. When loading metric extensions, the
// protos are mounted onto a virtual path inside this directory.
constexpr char kMetricProtoRoot[] = "protos/perfetto/metrics/";

// Enum which encodes how trace processor should try to sort the ingested data.
// Note that these options are only applicable to proto traces; other trace
// types (e.g. JSON, Fuchsia) use full sorts.
enum class SortingMode {
  // This option allows trace processor to use built-in heuristics about how to
  // sort the data. Generally, this option is correct for most embedders as
  // trace processor reads information from the trace to make the best decision.
  //
  // The exact heuristics are implementation details but will ensure that all
  // relevant tables are sorted by timestamp.
  //
  // This is the default mode.
  kDefaultHeuristics = 0,

  // This option forces trace processor to wait for all trace packets to be
  // passed to it before doing a full sort of all the packets. This causes any
  // heuristics trace processor would normally use to ingest partially sorted
  // data to be skipped.
  kForceFullSort = 1,

  // This option is deprecated in v18; trace processor will ignore it and
  // use |kDefaultHeuristics|.
  //
  // Rationale for deprecation:
  // The new windowed sorting logic in trace processor uses a combination of
  // flush and buffer-read lifecycle events inside the trace instead of
  // using time-periods from the config.
  //
  // Recommended migration:
  // Users of this option should switch to using |kDefaultHeuristics| which
  // will act very similarly to the pre-v20 behaviour of this option.
  //
  // This option is scheduled to be removed in v21.
  kForceFlushPeriodWindowedSort = 2
};

// Enum which encodes which event (if any) should be used to drop ftrace data
// from before this timestamp of that event.
enum class DropFtraceDataBefore {
  // Drops ftrace data before timestmap specified by the
  // TracingServiceEvent::tracing_started packet. If this packet is not in the
  // trace, no data is dropped.
  // Note: this event was introduced in S+ so no data will be dropped on R-
  // traces.
  // This is the default approach.
  kTracingStarted = 0,

  // Retains all ftrace data regardless of timestamp and other events.
  kNoDrop = 1,

  // Drops ftrace data before timestmap specified by the
  // TracingServiceEvent::all_data_sources_started. If this packet is not in the
  // trace, no data is dropped.
  // This option can be used in cases where R- traces are being considered and
  // |kTracingStart| cannot be used because the event was not present.
  kAllDataSourcesStarted = 2,
};

// Struct for configuring a TraceProcessor instance (see trace_processor.h).
struct PERFETTO_EXPORT Config {
  // Indicates the sortinng mode that trace processor should use on the passed
  // trace packets. See the enum documentation for more details.
  SortingMode sorting_mode = SortingMode::kDefaultHeuristics;

  // When set to false, this option makes the trace processor not include ftrace
  // events in the raw table; this makes converting events back to the systrace
  // text format impossible. On the other hand, it also saves ~50% of memory
  // usage of trace processor. For reference, Studio intends to use this option.
  //
  // Note: "generic" ftrace events will be parsed into the raw table even if
  // this flag is false and all other events which parse into the raw table are
  // unaffected by this flag.
  bool ingest_ftrace_in_raw_table = true;

  // Indicates the event which should be used as a marker to drop ftrace data in
  // the trace before that event. See the ennu documenetation for more details.
  DropFtraceDataBefore drop_ftrace_data_before =
      DropFtraceDataBefore::kTracingStarted;

  // Any built-in metric proto or sql files matching these paths are skipped
  // during trace processor metric initialization.
  std::vector<std::string> skip_builtin_metric_paths;
};

// Represents a dynamically typed value returned by SQL.
struct PERFETTO_EXPORT SqlValue {
  // Represents the type of the value.
  enum Type {
    kNull = 0,
    kLong,
    kDouble,
    kString,
    kBytes,
  };

  SqlValue() = default;

  static SqlValue Long(int64_t v) {
    SqlValue value;
    value.long_value = v;
    value.type = Type::kLong;
    return value;
  }

  static SqlValue Double(double v) {
    SqlValue value;
    value.double_value = v;
    value.type = Type::kDouble;
    return value;
  }

  static SqlValue String(const char* v) {
    SqlValue value;
    value.string_value = v;
    value.type = Type::kString;
    return value;
  }

  static SqlValue Bytes(const void* v, size_t size) {
    SqlValue value;
    value.bytes_value = v;
    value.bytes_count = size;
    value.type = Type::kBytes;
    return value;
  }

  double AsDouble() const {
    PERFETTO_CHECK(type == kDouble);
    return double_value;
  }
  int64_t AsLong() const {
    PERFETTO_CHECK(type == kLong);
    return long_value;
  }
  const char* AsString() const {
    PERFETTO_CHECK(type == kString);
    return string_value;
  }
  const void* AsBytes() const {
    PERFETTO_CHECK(type == kBytes);
    return bytes_value;
  }

  bool is_null() const { return type == Type::kNull; }

  // Up to 1 of these fields can be accessed depending on |type|.
  union {
    // This string will be owned by the iterator that returned it and is valid
    // as long until the subsequent call to Next().
    const char* string_value;
    int64_t long_value;
    double double_value;
    const void* bytes_value;
  };
  // The size of bytes_value. Only valid when |type == kBytes|.
  size_t bytes_count = 0;
  Type type = kNull;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_BASIC_TYPES_H_
