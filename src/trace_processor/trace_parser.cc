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

#include "src/trace_processor/trace_parser.h"

#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_processor {

using protozero::ProtoDecoder;
using protozero::proto_utils::kFieldTypeLengthDelimited;

namespace {
bool FindIntField(ProtoDecoder* decoder,
                  uint32_t field_id,
                  uint64_t* field_value) {
  for (auto f = decoder->ReadField(); f.id != 0; f = decoder->ReadField()) {
    if (f.id == field_id) {
      *field_value = f.int_value;
      return true;
    }
  }
  return false;
}
}  // namespace

TraceParser::TraceParser(BlobReader* reader,
                         TraceStorage* storage,
                         uint32_t chunk_size_b)
    : reader_(reader), storage_(storage), chunk_size_b_(chunk_size_b) {}

void TraceParser::ParseNextChunk() {
  if (!buffer_)
    buffer_.reset(new uint8_t[chunk_size_b_]);

  uint32_t read = reader_->Read(offset_, chunk_size_b_, buffer_.get());
  if (read == 0)
    return;

  ProtoDecoder decoder(buffer_.get(), read);
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    if (fld.id != protos::Trace::kPacketFieldNumber) {
      PERFETTO_ELOG("Non-trace packet field found in root Trace proto");
      continue;
    }
    ParsePacket(fld.length_limited.data,
                static_cast<uint32_t>(fld.length_limited.length));
  }

  offset_ += decoder.offset();
}

void TraceParser::ParsePacket(const uint8_t* data, uint32_t length) {
  ProtoDecoder decoder(data, length);
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::TracePacket::kFtraceEventsFieldNumber:
        ParseFtraceEventBundle(
            fld.length_limited.data,
            static_cast<uint32_t>(fld.length_limited.length));
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void TraceParser::ParseFtraceEventBundle(const uint8_t* data, uint32_t length) {
  ProtoDecoder decoder(data, length);
  uint64_t cpu = 0;
  if (!FindIntField(&decoder, protos::FtraceEventBundle::kCpuFieldNumber,
                    &cpu)) {
    PERFETTO_ELOG("CPU field not found in FtraceEventBundle");
    return;
  }
  decoder.Reset();

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::FtraceEventBundle::kEventFieldNumber:
        ParseFtraceEvent(static_cast<uint32_t>(cpu), fld.length_limited.data,
                         static_cast<uint32_t>(fld.length_limited.length));
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void TraceParser::ParseFtraceEvent(uint32_t cpu,
                                   const uint8_t* data,
                                   uint32_t length) {
  ProtoDecoder decoder(data, length);
  uint64_t timestamp = 0;
  if (!FindIntField(&decoder, protos::FtraceEvent::kTimestampFieldNumber,
                    &timestamp)) {
    PERFETTO_ELOG("Timestamp field not found in FtraceEvent");
    return;
  }
  decoder.Reset();

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::FtraceEvent::kSchedSwitchFieldNumber:
        PERFETTO_DCHECK(timestamp > 0);
        ParseSchedSwitch(cpu, timestamp, fld.length_limited.data,
                         static_cast<uint32_t>(fld.length_limited.length));
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void TraceParser::ParseSchedSwitch(uint32_t cpu,
                                   uint64_t timestamp,
                                   const uint8_t* data,
                                   uint32_t length) {
  ProtoDecoder decoder(data, length);

  uint32_t prev_pid = 0;
  uint32_t prev_state = 0;
  const char* prev_comm = nullptr;
  size_t prev_comm_len = 0;
  uint32_t next_pid = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SchedSwitchFtraceEvent::kPrevPidFieldNumber:
        prev_pid = fld.as_uint32();
        break;
      case protos::SchedSwitchFtraceEvent::kPrevStateFieldNumber:
        prev_state = fld.as_uint32();
        break;
      case protos::SchedSwitchFtraceEvent::kPrevCommFieldNumber:
        prev_comm = fld.as_char_ptr();
        prev_comm_len = fld.size();
        break;
      case protos::SchedSwitchFtraceEvent::kNextPidFieldNumber:
        next_pid = fld.as_uint32();
        break;
      default:
        break;
    }
  }
  storage_->PushSchedSwitch(cpu, timestamp, prev_pid, prev_state, prev_comm,
                            prev_comm_len, next_pid);

  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

}  // namespace trace_processor
}  // namespace perfetto