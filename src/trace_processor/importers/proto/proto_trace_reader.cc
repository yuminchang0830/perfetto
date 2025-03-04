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

#include "src/trace_processor/importers/proto/proto_trace_reader.h"

#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_module.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/proto/proto_incremental_state.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/gzip_utils.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/extension_descriptor.pbzero.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

ProtoTraceReader::ProtoTraceReader(TraceProcessorContext* ctx)
    : context_(ctx) {}
ProtoTraceReader::~ProtoTraceReader() = default;

util::Status ProtoTraceReader::Parse(std::unique_ptr<uint8_t[]> owned_buf,
                                     size_t size) {
  return tokenizer_.Tokenize(
      std::move(owned_buf), size,
      [this](TraceBlobView packet) { return ParsePacket(std::move(packet)); });
}

util::Status ProtoTraceReader::ParseExtensionDescriptor(ConstBytes descriptor) {
  protos::pbzero::ExtensionDescriptor::Decoder decoder(descriptor.data,
                                                       descriptor.size);

  auto extension = decoder.extension_set();
  return context_->descriptor_pool_->AddFromFileDescriptorSet(
      extension.data, extension.size,
      /*skip_prefixes*/ {},
      /*merge_existing_messages=*/true);
}

util::Status ProtoTraceReader::ParsePacket(TraceBlobView packet) {
  protos::pbzero::TracePacket::Decoder decoder(packet.data(), packet.length());
  if (PERFETTO_UNLIKELY(decoder.bytes_left())) {
    return util::ErrStatus(
        "Failed to parse proto packet fully; the trace is probably corrupt.");
  }

  // Any compressed packets should have been handled by the tokenizer.
  PERFETTO_CHECK(!decoder.has_compressed_packets());

  const uint32_t seq_id = decoder.trusted_packet_sequence_id();
  auto* state = GetIncrementalStateForPacketSequence(seq_id);

  uint32_t sequence_flags = decoder.sequence_flags();

  if (decoder.incremental_state_cleared() ||
      sequence_flags &
          protos::pbzero::TracePacket::SEQ_INCREMENTAL_STATE_CLEARED) {
    HandleIncrementalStateCleared(decoder);
  } else if (decoder.previous_packet_dropped()) {
    HandlePreviousPacketDropped(decoder);
  }

  // It is important that we parse defaults before parsing other fields such as
  // the timestamp, since the defaults could affect them.
  if (decoder.has_trace_packet_defaults()) {
    auto field = decoder.trace_packet_defaults();
    const size_t offset = packet.offset_of(field.data);
    ParseTracePacketDefaults(decoder, packet.slice(offset, field.size));
  }

  if (decoder.has_interned_data()) {
    auto field = decoder.interned_data();
    const size_t offset = packet.offset_of(field.data);
    ParseInternedData(decoder, packet.slice(offset, field.size));
  }

  if (decoder.has_clock_snapshot()) {
    return ParseClockSnapshot(decoder.clock_snapshot(),
                              decoder.trusted_packet_sequence_id());
  }

  if (decoder.has_service_event()) {
    PERFETTO_DCHECK(decoder.has_timestamp());
    int64_t ts = static_cast<int64_t>(decoder.timestamp());
    return ParseServiceEvent(ts, decoder.service_event());
  }

  if (decoder.has_extension_descriptor()) {
    return ParseExtensionDescriptor(decoder.extension_descriptor());
  }

  if (decoder.sequence_flags() &
      protos::pbzero::TracePacket::SEQ_NEEDS_INCREMENTAL_STATE) {
    if (!seq_id) {
      return util::ErrStatus(
          "TracePacket specified SEQ_NEEDS_INCREMENTAL_STATE but the "
          "TraceWriter's sequence_id is zero (the service is "
          "probably too old)");
    }

    if (!state->IsIncrementalStateValid()) {
      context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
      return util::OkStatus();
    }
  }

  // Workaround a bug in the frame timeline traces which is emitting packets
  // with zero timestamp (b/179905685).
  // TODO(primiano): around mid-2021 there should be no traces that have this
  // bug and we should be able to remove this workaround.
  if (decoder.has_frame_timeline_event() && decoder.timestamp() == 0) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return util::OkStatus();
  }

  protos::pbzero::TracePacketDefaults::Decoder* defaults =
      state->current_generation()->GetTracePacketDefaults();

  int64_t timestamp;
  if (decoder.has_timestamp()) {
    timestamp = static_cast<int64_t>(decoder.timestamp());

    uint32_t timestamp_clock_id =
        decoder.has_timestamp_clock_id()
            ? decoder.timestamp_clock_id()
            : (defaults ? defaults->timestamp_clock_id() : 0);

    if ((decoder.has_chrome_events() || decoder.has_chrome_metadata()) &&
        (!timestamp_clock_id ||
         timestamp_clock_id == protos::pbzero::BUILTIN_CLOCK_MONOTONIC)) {
      // Chrome event timestamps are in MONOTONIC domain, but may occur in
      // traces where (a) no clock snapshots exist or (b) no clock_id is
      // specified for their timestamps. Adjust to trace time if we have a clock
      // snapshot.
      // TODO(eseckler): Set timestamp_clock_id and emit ClockSnapshots in
      // chrome and then remove this.
      auto trace_ts = context_->clock_tracker->ToTraceTime(
          protos::pbzero::BUILTIN_CLOCK_MONOTONIC, timestamp);
      if (trace_ts.has_value())
        timestamp = trace_ts.value();
    } else if (timestamp_clock_id) {
      // If the TracePacket specifies a non-zero clock-id, translate the
      // timestamp into the trace-time clock domain.
      ClockTracker::ClockId converted_clock_id = timestamp_clock_id;
      bool is_seq_scoped =
          ClockTracker::IsReservedSeqScopedClockId(converted_clock_id);
      if (is_seq_scoped) {
        if (!seq_id) {
          return util::ErrStatus(
              "TracePacket specified a sequence-local clock id (%" PRIu32
              ") but the TraceWriter's sequence_id is zero (the service is "
              "probably too old)",
              timestamp_clock_id);
        }
        converted_clock_id =
            ClockTracker::SeqScopedClockIdToGlobal(seq_id, timestamp_clock_id);
      }
      auto trace_ts =
          context_->clock_tracker->ToTraceTime(converted_clock_id, timestamp);
      if (!trace_ts.has_value()) {
        // ToTraceTime() will increase the |clock_sync_failure| stat on failure.
        // We don't return an error here as it will cause the trace to stop
        // parsing. Instead, we rely on the stat increment in ToTraceTime() to
        // inform the user about the error.
        return util::OkStatus();
      }
      timestamp = trace_ts.value();
    }
  } else {
    timestamp = std::max(latest_timestamp_, context_->sorter->max_timestamp());
  }
  latest_timestamp_ = std::max(timestamp, latest_timestamp_);

  auto& modules = context_->modules_by_field;
  for (uint32_t field_id = 1; field_id < modules.size(); ++field_id) {
    if (!modules[field_id].empty() && decoder.Get(field_id).valid()) {
      for (ProtoImporterModule* module : modules[field_id]) {
        ModuleResult res = module->TokenizePacket(decoder, &packet, timestamp,
                                                  state, field_id);
        if (!res.ignored())
          return res.ToStatus();
      }
    }
  }

  if (decoder.has_trace_config()) {
    ParseTraceConfig(decoder.trace_config());
  }

  // Use parent data and length because we want to parse this again
  // later to get the exact type of the packet.
  context_->sorter->PushTracePacket(timestamp, state, std::move(packet));

  return util::OkStatus();
}

void ProtoTraceReader::ParseTraceConfig(protozero::ConstBytes blob) {
  protos::pbzero::TraceConfig::Decoder trace_config(blob);
  if (trace_config.write_into_file() && !trace_config.flush_period_ms()) {
    PERFETTO_ELOG(
        "It is strongly recommended to have flush_period_ms set when "
        "write_into_file is turned on. This trace will be loaded fully "
        "into memory before sorting which increases the likliehoold of "
        "OOMs.");
  }
}

void ProtoTraceReader::HandleIncrementalStateCleared(
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG(
        "incremental_state_cleared without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }
  GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id())
      ->OnIncrementalStateCleared();
  for (auto& module : context_->modules) {
    module->OnIncrementalStateCleared(
        packet_decoder.trusted_packet_sequence_id());
  }
}

void ProtoTraceReader::HandlePreviousPacketDropped(
    const protos::pbzero::TracePacket::Decoder& packet_decoder) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("previous_packet_dropped without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }
  GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id())
      ->OnPacketLoss();
}

void ProtoTraceReader::ParseTracePacketDefaults(
    const protos::pbzero::TracePacket_Decoder& packet_decoder,
    TraceBlobView trace_packet_defaults) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG(
        "TracePacketDefaults packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }

  auto* state = GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id());
  state->UpdateTracePacketDefaults(std::move(trace_packet_defaults));
}

void ProtoTraceReader::ParseInternedData(
    const protos::pbzero::TracePacket::Decoder& packet_decoder,
    TraceBlobView interned_data) {
  if (PERFETTO_UNLIKELY(!packet_decoder.has_trusted_packet_sequence_id())) {
    PERFETTO_ELOG("InternedData packet without trusted_packet_sequence_id");
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    return;
  }

  auto* state = GetIncrementalStateForPacketSequence(
      packet_decoder.trusted_packet_sequence_id());

  // Don't parse interned data entries until incremental state is valid, because
  // they could otherwise be associated with the wrong generation in the state.
  if (!state->IsIncrementalStateValid()) {
    context_->storage->IncrementStats(stats::tokenizer_skipped_packets);
    return;
  }

  // Store references to interned data submessages into the sequence's state.
  protozero::ProtoDecoder decoder(interned_data.data(), interned_data.length());
  for (protozero::Field f = decoder.ReadField(); f.valid();
       f = decoder.ReadField()) {
    auto bytes = f.as_bytes();
    auto offset = interned_data.offset_of(bytes.data);
    state->InternMessage(f.id(), interned_data.slice(offset, bytes.size));
  }
}

util::Status ProtoTraceReader::ParseClockSnapshot(ConstBytes blob,
                                                  uint32_t seq_id) {
  std::vector<ClockTracker::ClockValue> clocks;
  protos::pbzero::ClockSnapshot::Decoder evt(blob.data, blob.size);
  if (evt.primary_trace_clock()) {
    context_->clock_tracker->SetTraceTimeClock(
        static_cast<ClockTracker::ClockId>(evt.primary_trace_clock()));
  }
  for (auto it = evt.clocks(); it; ++it) {
    protos::pbzero::ClockSnapshot::Clock::Decoder clk(*it);
    ClockTracker::ClockId clock_id = clk.clock_id();
    if (ClockTracker::IsReservedSeqScopedClockId(clk.clock_id())) {
      if (!seq_id) {
        return util::ErrStatus(
            "ClockSnapshot packet is specifying a sequence-scoped clock id "
            "(%" PRIu64 ") but the TracePacket sequence_id is zero",
            clock_id);
      }
      clock_id = ClockTracker::SeqScopedClockIdToGlobal(seq_id, clk.clock_id());
    }
    int64_t unit_multiplier_ns =
        clk.unit_multiplier_ns()
            ? static_cast<int64_t>(clk.unit_multiplier_ns())
            : 1;
    clocks.emplace_back(clock_id, clk.timestamp(), unit_multiplier_ns,
                        clk.is_incremental());
  }

  uint32_t snapshot_id = context_->clock_tracker->AddSnapshot(clocks);

  // Add the all the clock values to the clock snapshot table.
  base::Optional<int64_t> trace_ts_for_check;
  for (const auto& clock : clocks) {
    // If the clock is incremental, we need to use 0 to map correctly to
    // |absolute_timestamp|.
    int64_t ts_to_convert = clock.is_incremental ? 0 : clock.absolute_timestamp;
    base::Optional<int64_t> opt_trace_ts =
        context_->clock_tracker->ToTraceTime(clock.clock_id, ts_to_convert);
    if (!opt_trace_ts) {
      // This can happen if |AddSnapshot| failed to resolve this clock. Just
      // ignore this and move on.
      continue;
    }

    // Double check that all the clocks in this snapshot resolve to the same
    // trace timestamp value.
    PERFETTO_DCHECK(!trace_ts_for_check || opt_trace_ts == trace_ts_for_check);
    trace_ts_for_check = *opt_trace_ts;

    tables::ClockSnapshotTable::Row row;
    row.ts = *opt_trace_ts;
    row.clock_id = static_cast<int64_t>(clock.clock_id);
    row.clock_value = clock.absolute_timestamp;
    row.clock_name = GetBuiltinClockNameOrNull(clock.clock_id);
    row.snapshot_id = snapshot_id;

    auto* snapshot_table = context_->storage->mutable_clock_snapshot_table();
    snapshot_table->Insert(row);
  }
  return util::OkStatus();
}

base::Optional<StringId> ProtoTraceReader::GetBuiltinClockNameOrNull(
    uint64_t clock_id) {
  switch (clock_id) {
    case protos::pbzero::ClockSnapshot::Clock::REALTIME:
      return context_->storage->InternString("REALTIME");
    case protos::pbzero::ClockSnapshot::Clock::REALTIME_COARSE:
      return context_->storage->InternString("REALTIME_COARSE");
    case protos::pbzero::ClockSnapshot::Clock::MONOTONIC:
      return context_->storage->InternString("MONOTONIC");
    case protos::pbzero::ClockSnapshot::Clock::MONOTONIC_COARSE:
      return context_->storage->InternString("MONOTONIC_COARSE");
    case protos::pbzero::ClockSnapshot::Clock::MONOTONIC_RAW:
      return context_->storage->InternString("MONOTONIC_RAW");
    case protos::pbzero::ClockSnapshot::Clock::BOOTTIME:
      return context_->storage->InternString("BOOTTIME");
    default:
      return base::nullopt;
  }
}

util::Status ProtoTraceReader::ParseServiceEvent(int64_t ts, ConstBytes blob) {
  protos::pbzero::TracingServiceEvent::Decoder tse(blob);
  if (tse.tracing_started()) {
    context_->metadata_tracker->SetMetadata(metadata::tracing_started_ns,
                                            Variadic::Integer(ts));
  }
  if (tse.tracing_disabled()) {
    context_->metadata_tracker->SetMetadata(metadata::tracing_disabled_ns,
                                            Variadic::Integer(ts));
  }
  if (tse.all_data_sources_started()) {
    context_->metadata_tracker->SetMetadata(
        metadata::all_data_source_started_ns, Variadic::Integer(ts));
  }
  if (tse.all_data_sources_flushed()) {
    context_->sorter->NotifyFlushEvent();
  }
  if (tse.read_tracing_buffers_completed()) {
    context_->sorter->NotifyReadBufferEvent();
  }
  return util::OkStatus();
}

void ProtoTraceReader::NotifyEndOfFile() {}

}  // namespace trace_processor
}  // namespace perfetto
