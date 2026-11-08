/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2024 Xenia Canary. All rights reserved.                          *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include "xenia/apu/xma_context_new.h"
#include "xenia/apu/xma_helpers.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"

extern "C" {
#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#include "third_party/FFmpeg/libavutil/channel_layout.h"
#include "third_party/FFmpeg/libavutil/error.h"
#if XE_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

namespace xe {
namespace apu {

XmaContextNew::XmaContextNew() = default;

XmaContextNew::~XmaContextNew() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContextNew::Setup(uint32_t id, Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    XELOGE("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    XELOGE("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->ch_layout = AVChannelLayout{};
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    XELOGE("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

RingBuffer XmaContextNew::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity =
      data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset =
      data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset =
      data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    XELOGW(
        "XmaContext {}: Output buffer uses more space than expected! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  // Output buffers are in raw PCM samples, 256 bytes per block.
  // Output buffer is a ring buffer. We need to write from the write offset
  // to the read offset.
  RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      (int32_t)output_rb.write_count() / kOutputBytesPerBlock;

  return output_rb;
}

bool XmaContextNew::Work() {
  if (!is_enabled() || !is_allocated()) {
    return false;
  }

  std::lock_guard<xe_mutex> lock(lock_);
  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    SetError(&data, kErrorOutputFull);
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  // A frame part way out is held between kicks. If the guest moved its
  // position, decoding starts again from where the position now points.
  if (current_frame_remaining_subframes_ &&
      (data.input_buffer_read_offset != held_offset_ ||
       data.decode_subframe != held_subframe_ ||
       data.current_buffer != held_buffer_)) {
    XELOGAPU("XmaContext {}: Position moved, dropping the held frame", id());
    current_frame_remaining_subframes_ = 0;
    advance_ = {};
  }

  RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  if (data.IsConsumeOnlyContext()) {
    // Nothing to drain — don't touch the context or we'll reset the
    // game's output buffer offsets, causing stale PCM to be re-read.
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    XELOGAPU("XmaContext {}: Consume-only context, draining subframes", id());
    Consume(&output_rb, &data);
    if (!current_frame_remaining_subframes_) {
      ApplyAdvance(&data);
    }
    data.output_buffer_write_offset =
        output_rb.write_offset() / kOutputBytesPerBlock;

    if (output_rb.empty()) {
      data.output_buffer_valid = false;
    }
    WritePosition(&data);
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  // A pass only starts when there is room for it.
  if (static_cast<int32_t>(GetPassBlocks(&data)) >
      remaining_subframe_blocks_in_output_buffer_) {
    XELOGAPU("XmaContext {}: No space for subframe decoding {}/{}!", id(),
             GetPassBlocks(&data), remaining_subframe_blocks_in_output_buffer_);
    SetError(&data, kErrorOutputSpace);
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  // Frames left in the pass being written. A pass only starts when there is
  // room for all of it, and ends early where the advance says.
  uint32_t pass_frames = 0;
  while (true) {
    if (!pass_frames &&
        static_cast<int32_t>(GetPassBlocks(&data, &pass_frames)) >
            remaining_subframe_blocks_in_output_buffer_) {
      break;
    }
    XELOGAPU(
        "XmaContext {}: Write Count: {}, Capacity: {} - {} {} Subframes: {} "
        "Decode subframe: {}",
        id(), (uint32_t)output_rb.write_count(),
        remaining_subframe_blocks_in_output_buffer_,
        data.input_buffer_0_valid + (data.input_buffer_1_valid << 1),
        data.output_buffer_valid, data.subframe_decode_count,
        data.decode_subframe);

    const uint32_t pre_decode_offset = data.input_buffer_read_offset;
    const uint32_t pre_decode_subframe = data.decode_subframe;
    const uint32_t pre_buffer = data.current_buffer;
    const int32_t pre_free_blocks = remaining_subframe_blocks_in_output_buffer_;

    decode_stop_ = kDecodeStop::kNone;
    frame_skipped_ = false;
    Decode(&data);
    Consume(&output_rb, &data);
    if (!current_frame_remaining_subframes_) {
      const bool ends_pass = advance_.ends_pass_;
      ApplyAdvance(&data);
      if (!frame_skipped_) {
        pass_frames = ends_pass ? 0 : pass_frames - 1;
      }
    } else {
      pass_frames = 0;
    }

    // Don't abandon a partially consumed frame. Consume() hands over at most
    // subframe_decode_count blocks per pass so the pass that exhausts the
    // input typically leaves the rest of the frame undelivered. Work() has
    // already cleared is_enabled_ and only XMAEnableContext sets it again.
    // Game polling for the remainder therefore never kicks and nothing
    // else can ever deliver it so keep draining until the frame is out.
    if ((!data.IsAnyInputBufferValid() || decode_stop_ != kDecodeStop::kNone) &&
        current_frame_remaining_subframes_ == 0) {
      XELOGAPU("XmaContext {}: Work loop exit - buffers_valid={} stop={}", id(),
               data.IsAnyInputBufferValid(), static_cast<int>(decode_stop_));
      break;
    }

    // Nothing written and the position where it was means decoding can't go
    // on. A loop jump back to the frame it came from writes and moves the
    // position's subframe, so it isn't a stall.
    if (remaining_subframe_blocks_in_output_buffer_ == pre_free_blocks &&
        data.input_buffer_read_offset == pre_decode_offset &&
        data.decode_subframe == pre_decode_subframe &&
        data.current_buffer == pre_buffer) {
      XELOGAPU(
          "XmaContext {}: Decode stalled at offset {} (no progress), "
          "waiting for next buffer",
          id(), pre_decode_offset);
      break;
    }
  }

  // The status says why decoding stopped.
  const bool frame_done = !current_frame_remaining_subframes_;
  if (frame_done && decode_stop_ == kDecodeStop::kWaitForBuffer) {
    SetError(&data, kErrorWaitForBuffer);
  } else if (frame_done && decode_stop_ == kDecodeStop::kParserError) {
    // The parser status says why.
  } else if (frame_done && !data.IsAnyInputBufferValid()) {
    SetError(&data, kErrorNoInput);
  } else if (remaining_subframe_blocks_in_output_buffer_ <
             static_cast<int32_t>(GetPassBlocks(&data))) {
    SetError(&data, remaining_subframe_blocks_in_output_buffer_
                        ? kErrorOutputSpace
                        : kErrorOutputFull);
  }

  data.output_buffer_write_offset =
      output_rb.write_offset() / kOutputBytesPerBlock;

  // Signal the game that the output buffer is full
  if (remaining_subframe_blocks_in_output_buffer_ == 0 && output_rb.empty()) {
    XELOGAPU("XmaContext {}: Output ring buffer is full, invalidating output",
             id());
    data.output_buffer_valid = 0;
  }

  XELOGAPU("XmaContext {}: Read Output: {} Write Output: {}", id(),
           data.output_buffer_read_offset, data.output_buffer_write_offset);

  WritePosition(&data);
  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

void XmaContextNew::Enable() { set_is_enabled(true); }

void XmaContextNew::Clear() {
  std::lock_guard<xe_mutex> lock(lock_);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XmaContextNew::ClearLocked(XMA_CONTEXT_DATA* data) {
  XELOGAPU("XmaContext: reset context {}", id());

  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  current_frame_remaining_subframes_ = 0;
  advance_ = {};
  FlushDecoder();
}

void XmaContextNew::FlushDecoder() {
  // The next frame starts a new stream, which must not get the previous
  // stream's overlap.
  if (av_context_ && avcodec_is_open(av_context_)) {
    avcodec_flush_buffers(av_context_);
  }
}

void XmaContextNew::Disable() { set_is_enabled(false); }

void XmaContextNew::Release() {
  // Lock it in case the decoder thread is working on it now.
  std::lock_guard<xe_mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  FlushDecoder();
}

int XmaContextNew::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

void XmaContextNew::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  // No more frames.
  XELOGAPU("XmaContext: SwapInputBuffer from buffer {} to {}",
           data->current_buffer, data->current_buffer ^ 1);
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContextNew::Consume(RingBuffer* XE_RESTRICT output_rb,
                            const XMA_CONTEXT_DATA* const XE_RESTRICT data) {
  if (!current_frame_remaining_subframes_) {
    return;
  }

  // Guard against subframe_decode_count == 0 which would cause zero progress
  // and an infinite loop in Work().  Treat 0 as 1 (minimum progress).
  const uint8_t effective_sdc =
      std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  // A pass writes whole subframes, two blocks each in stereo.
  const int8_t subframes_to_write =
      std::min((int8_t)current_frame_remaining_subframes_,
               (int8_t)(effective_sdc << data->is_stereo));

  const int8_t raw_frame_read_offset =
      current_frame_end_block_ - current_frame_remaining_subframes_;

  output_rb->Write(
      raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
      subframes_to_write * kOutputBytesPerBlock);

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write;
  current_frame_remaining_subframes_ -= subframes_to_write;

  XELOGAPU("XmaContext {}: Consume: {} - {} - {} - {} - {}", id(),
           remaining_subframe_blocks_in_output_buffer_,
           data->output_buffer_write_offset, data->output_buffer_read_offset,
           output_rb->write_offset(), current_frame_remaining_subframes_);
}

void XmaContextNew::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  // No available data.
  if (!data->IsAnyInputBufferValid()) {
    XELOGAPU("XmaContext {}: Decode skipped - no valid input buffers", id());
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    XELOGAPU("XmaContext {}: Current buffer {} invalid, swapping to other",
             id(), data->current_buffer);
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      XELOGAPU("XmaContext {}: Both buffers invalid after swap, aborting",
               id());
      return;
    }
    const uint32_t first_frame_offset =
        xma::GetPacketFrameOffset(GetCurrentInputBuffer(data));
    if (first_frame_offset <= kMaxFrameSizeinBits) {
      data->input_buffer_read_offset = first_frame_offset;
    }
  }

  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);

  input_buffer_.fill(0);

  // The frame at loop_end is cut after loop_subframe_end, then decoding
  // jumps to loop_start. Starting past loop_subframe_end plays it to its end
  // and decoding goes on from there, even into padding.
  const bool at_loop_end =
      data->loop_count > 0 && data->input_buffer_read_offset == data->loop_end;
  const bool is_loop_end_frame = IsLoopEndFrame(data);

  if (!data->output_buffer_block_count) {
    XELOGE("XmaContext {}: Error - Received 0 for output_buffer_block_count!",
           id());
    return;
  }

  XELOGAPU(
      "Processing context {} (offset {}, buffer {}, ptr {:p}, output buffer "
      "{:08X}, output buffer count {})",
      id(), data->input_buffer_read_offset, data->current_buffer,
      static_cast<void*>(current_input_buffer), data->output_buffer_ptr,
      data->output_buffer_block_count);

  // No frame starts in a packet header.
  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    XELOGAPU("XmaContext {}: Read offset {} is inside the packet header", id(),
             data->input_buffer_read_offset);
    StopOnParserError(data, kParserErrorNoFrame);
    return;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count =
      current_input_size / kBytesPerPacket;

  const int16_t packet_index =
      GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    XELOGE("XmaContext {}: Invalid packet index. Input read offset: {}", id(),
           data->input_buffer_read_offset);
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  const uint32_t relative_offset =
      data->input_buffer_read_offset % kBitsPerPacket;

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip — no new frames begin in this packet (XMA2: 0xFF,
  // XMA1: lower 8 bits of 0x7FF also reads as 0xFF).  Advance to the
  // next sequential packet instead of trying to parse frames.
  if (skip_count == 0xFF) {
    XELOGAPU("XmaContext {}: Full packet skip (0xFF) at packet {}/{}", id(),
             packet_index, current_input_packet_count);
    const kNextFrame next =
        FindNextFrame(data, packet_index + 1, current_input_packet_count);
    for (uint32_t i = 0; i < next.buffers_consumed_; ++i) {
      SwapInputBuffer(data);
    }
    data->input_buffer_read_offset = next.offset_;
    return;
  }

  // Before the packet's first frame is the tail of a frame from the previous
  // packet. No frame starts there.
  if (relative_offset < packet_first_frame_offset) {
    XELOGAPU("XmaContext {}: Read offset {} is in a split frame tail", id(),
             data->input_buffer_read_offset);
    StopOnParserError(data, kParserErrorNoFrame);
    return;
  }

  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  if (!packet_info.frame_found_) {
    // Padding after a packet's last frame reads as a frame running into the
    // next packet, so it fails only once that packet is there.
    BitStream header(packet, kBitsPerPacket);
    header.SetOffset(relative_offset);
    if (header.BitsRemaining() >= kBitsPerFrameHeader &&
        header.Peek(kBitsPerFrameHeader) == xma::kMaxFrameLength &&
        !GetNextPacket(data, next_packet_index, current_input_packet_count)) {
      XELOGAPU(
          "XmaContext {}: Read offset {} is in padding, waiting for the "
          "next buffer",
          id(), data->input_buffer_read_offset);
      decode_stop_ = kDecodeStop::kWaitForBuffer;
      return;
    }
    XELOGAPU("XmaContext {}: No frame starts at read offset {}", id(),
             data->input_buffer_read_offset);
    StopOnParserError(data, kParserErrorNoFrame);
    return;
  }

  // Frame header split across packet boundary — combine packets to read
  // the full 15-bit header and resolve the real frame size.
  if (packet_info.current_frame_size_ == 0) {
    XELOGAPU(
        "XmaContext {}: Split frame header at packet {} boundary, "
        "combining with next packet {}",
        id(), packet_index, next_packet_index);
    const uint8_t* next_packet =
        GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      // Hold the buffer until the one the header continues in is valid.
      XELOGAPU(
          "XmaContext {}: Split frame header at packet {}, waiting for the "
          "next buffer",
          id(), packet_index);
      decode_stop_ = kDecodeStop::kWaitForBuffer;
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader,
                kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData,
                next_packet + kBytesPerPacketHeader, kBytesPerPacketData);

    BitStream combined(input_buffer_.data(),
                       (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      XELOGW("XmaContext {}: Split header resolved to kMaxFrameLength (0x7FFF)",
             id());
      StopOnParserError(data, kParserErrorNoFrame);
      return;
    }
    packet_info.current_frame_size_ = (uint32_t)frame_size;
  }

  BitStream stream =
      BitStream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy = GetAmountOfBitsToRead(
      (uint32_t)stream.BitsRemaining(), packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    XELOGE("XmaContext {}: There is no bits to copy!", id());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    // Frame is a splitted frame
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);

      if (!next_packet) {
        // Hold the buffer until the one the frame continues in is valid.
        decode_stop_ = kDecodeStop::kWaitForBuffer;
        return;
      }
      // Copy next packet to buffer
      std::memcpy(input_buffer_.data() + kBytesPerPacketData,
                  next_packet + kBytesPerPacketHeader, kBytesPerPacketData);
    }
  }

  // Copy current packet to buffer
  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader,
              kBytesPerPacketData);

  stream = BitStream(input_buffer_.data(),
                     (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  XELOGAPU(
      "XmaContext {}: Reading Frame {}/{} (size: {}) From Packet "
      "{}/{}",
      id(), (int32_t)packet_info.current_frame_, packet_info.frame_count_,
      packet_info.current_frame_size_, packet_index,
      current_input_packet_count);

  const uint32_t padding_start = static_cast<uint8_t>(
      stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  raw_frame_.fill(0);

  PrepareDecoder(data->sample_rate, bool(data->is_stereo));
  // A loop end frame cut before its last subframe leaves the loop start to
  // overlap the frame before it.
  PreparePacket(packet_info.current_frame_size_, padding_start,
                is_loop_end_frame && data->loop_subframe_end < 3);
  const kPacketResult result = DecodePacket(av_context_, av_packet_, av_frame_);
  if (result == kPacketResult::kError) {
    // The position stays on the frame, which fails again until the guest
    // moves it. The flush lets the codec take a frame again after that.
    StopOnParserError(data, kParserErrorBitstream);
    FlushDecoder();
    return;
  }
  if (result == kPacketResult::kFrame) {
    data->parser_error_status = 0;
    data->parser_error_set = 0;
    // dump_raw(av_frame_, id());
    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data),
                 bool(data->is_stereo), raw_frame_.data());
    // Output runs from the decode position's subframe to the end of the
    // frame, or to loop_subframe_end on a loop end frame. A position of 4 or
    // more skips the frame.
    const uint32_t end_subframe =
        is_loop_end_frame ? data->loop_subframe_end + 1u : 4u;
    const uint32_t first_subframe =
        std::min<uint32_t>(data->decode_subframe, end_subframe);
    current_frame_end_block_ =
        static_cast<uint8_t>(end_subframe << data->is_stereo);
    current_frame_remaining_subframes_ = static_cast<uint8_t>(
        (end_subframe - first_subframe) << data->is_stereo);
    frame_skipped_ = !current_frame_remaining_subframes_;
    data->decode_subframe = 0;
  }

  if (is_loop_end_frame) {
    AdvanceAfterFrame(data, 0, 0, true);
    return;
  }

  if (at_loop_end &&
      relative_offset + packet_info.current_frame_size_ <= kBitsPerPacket) {
    AdvanceAfterFrame(
        data, data->input_buffer_read_offset + packet_info.current_frame_size_,
        0, false);
    return;
  }

  // Compute where to go next.
  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;

    XELOGAPU("XmaContext {}: Index: {}/{} - Next frame offset: {}", id(),
             (int32_t)packet_info.current_frame_, packet_info.frame_count_,
             next_frame_offset);

    AdvanceAfterFrame(data, (packet_index * kBitsPerPacket) + next_frame_offset,
                      0, false);
    return;
  }

  const uint8_t* next_packet =
      GetNextPacket(data, next_packet_index, current_input_packet_count);

  if (!next_packet) {
    XELOGAPU(
        "XmaContext {}: Last frame in packet {}, next packet {} unavailable "
        "(end of buffer)",
        id(), packet_index, next_packet_index);
  }

  const uint32_t packet_bits_left = kBitsPerPacket - relative_offset;
  const uint32_t split_tail_bits =
      packet_info.current_frame_size_ > packet_bits_left
          ? packet_info.current_frame_size_ - packet_bits_left
          : 0;
  const kNextFrame next = FindNextFrame(
      data, next_packet_index, current_input_packet_count, split_tail_bits);
  XELOGAPU("XmaContext {}: Next frame at {} after {} buffers", id(),
           next.offset_, next.buffers_consumed_);
  // A frame split into the other buffer leaves its buffer part way through.
  const bool split_into_other_buffer =
      split_tail_bits && next_packet_index >= current_input_packet_count;
  AdvanceAfterFrame(data, next.offset_, next.buffers_consumed_, false,
                    next.buffers_consumed_ && !split_into_other_buffer);
}

void XmaContextNew::AdvanceAfterFrame(XMA_CONTEXT_DATA* data, uint32_t offset,
                                      uint32_t buffers_consumed, bool loop_jump,
                                      bool ends_pass) {
  advance_.pending_ = true;
  advance_.loop_jump_ = loop_jump;
  advance_.ends_pass_ = loop_jump || ends_pass;
  advance_.offset_ = offset;
  advance_.buffers_consumed_ = buffers_consumed;
  if (!current_frame_remaining_subframes_) {
    ApplyAdvance(data);
  }
}

void XmaContextNew::ApplyAdvance(XMA_CONTEXT_DATA* data) {
  if (!advance_.pending_) {
    return;
  }
  const kPendingAdvance advance = advance_;
  advance_ = {};
  // The next frame starts at its first subframe.
  data->decode_subframe = 0;
  if (advance.loop_jump_) {
    UpdateLoopStatus(data);
    return;
  }
  for (uint32_t i = 0; i < advance.buffers_consumed_; ++i) {
    SwapInputBuffer(data);
  }
  data->input_buffer_read_offset = advance.offset_;
}

void XmaContextNew::WritePosition(XMA_CONTEXT_DATA* data) {
  // Output resumes at the next subframe of the frame, counting any it
  // skipped. A finished frame leaves decode_subframe alone: 0, or a seek that
  // hasn't been decoded yet.
  if (current_frame_remaining_subframes_) {
    data->decode_subframe =
        (current_frame_end_block_ - current_frame_remaining_subframes_) >>
        data->is_stereo;
  }
  held_offset_ = data->input_buffer_read_offset;
  held_subframe_ = data->decode_subframe;
  held_buffer_ = data->current_buffer;
}

//  Frame & Packet searching methods

void XmaContextNew::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }

  XELOGAPU("XmaContext {}: Looped Data: {} < {} (Start: {}) Remaining: {}",
           id(), data->input_buffer_read_offset, data->loop_end,
           data->loop_start, data->loop_count);

  if (data->input_buffer_read_offset != data->loop_end) {
    return;
  }

  // Decoding goes on at loop_start from subframe loop_subframe_skip. A
  // loop_start of 0 stops on the packet header.
  data->input_buffer_read_offset = data->loop_start;
  data->decode_subframe = data->loop_subframe_skip;

  if (data->loop_count != 255) {
    data->loop_count--;
  }
}

bool XmaContextNew::IsLoopEndFrame(const XMA_CONTEXT_DATA* data) {
  // A loop_end of 0 is never a frame's offset, so it never loops.
  return data->loop_count > 0 &&
         data->input_buffer_read_offset == data->loop_end &&
         data->decode_subframe <= data->loop_subframe_end;
}

uint32_t XmaContextNew::GetPassBlocks(const XMA_CONTEXT_DATA* data,
                                      uint32_t* frames) const {
  const uint32_t subframe =
      current_frame_remaining_subframes_
          ? static_cast<uint32_t>(current_frame_end_block_ -
                                  current_frame_remaining_subframes_) >>
                data->is_stereo
          : data->decode_subframe;
  const uint32_t rest = subframe < 4 ? 4 - subframe : 4;
  const uint32_t sdc =
      std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  const uint32_t next = rest <= sdc && sdc - rest >= 4 ? 1 : 0;
  if (frames) {
    *frames = 1 + next;
  }
  return (rest > sdc ? sdc : rest + 4 * next) << data->is_stereo;
}

kPacketHandle XmaContextNew::GetPacketHandle(
    XMA_CONTEXT_DATA* data, uint32_t buffer_index, uint32_t packet_index,
    uint32_t current_input_packet_count) {
  kPacketHandle result{};
  bool is_packet_in_next_buffer = packet_index >= current_input_packet_count;
  if (is_packet_in_next_buffer) {
    buffer_index = buffer_index ^ 1;
    packet_index = packet_index - current_input_packet_count;
  }

  if (!data->IsInputBufferValid(buffer_index)) {
    return result;
  }

  const uint32_t next_buffer_address =
      data->GetInputBufferAddress(buffer_index);
  if (!next_buffer_address) {
    // This should never occur but there is always a chance
    XELOGE(
        "XmaContext {}: the packet is expected to be in the {} buffer, "
        "but the buffer marked valid is null pointer!",
        id(), is_packet_in_next_buffer ? "next" : "current");
    return result;
  }

  const uint32_t next_buffer_packet_count =
      data->GetInputBufferPacketCount(buffer_index);
  if (packet_index >= next_buffer_packet_count) {
    XELOGE(
        "XmaContext {}: the packet is expected to be in the {} buffer, "
        "but the buffer is too short to contain this packet!",
        id(), is_packet_in_next_buffer ? "next" : "current");
    return result;
  }

  result.buffer_index_ = buffer_index;
  result.packet_index_ = packet_index;
  result.is_valid_ = true;
  return result;
}

const uint8_t* XmaContextNew::GetNextPacket(
    XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
    uint32_t current_input_packet_count) {
  kPacketHandle packet_handle =
      GetPacketHandle(data, data->current_buffer, next_packet_index,
                      current_input_packet_count);
  if (!packet_handle.is_valid_) {
    return nullptr;
  }

  const uint32_t next_buffer_address =
      data->GetInputBufferAddress(packet_handle.buffer_index_);
  return memory()->TranslatePhysical(next_buffer_address) +
         packet_handle.packet_index_ * kBytesPerPacket;
}

kNextFrame XmaContextNew::FindNextFrame(XMA_CONTEXT_DATA* data,
                                        uint32_t next_packet_index,
                                        uint32_t current_input_packet_count,
                                        uint32_t split_tail_bits) {
  kNextFrame result;
  uint32_t buffer_index = data->current_buffer;
  uint32_t packet_index = next_packet_index;
  uint32_t packet_count = current_input_packet_count;
  while (true) {
    if (packet_index >= packet_count) {
      if (++result.buffers_consumed_ == 2) {
        result.offset_ = kBitsPerPacketHeader;
        return result;
      }
      buffer_index ^= 1;
      packet_index -= packet_count;
      packet_count = data->GetInputBufferPacketCount(buffer_index);
      if (!data->IsInputBufferValid(buffer_index) ||
          !data->GetInputBufferAddress(buffer_index)) {
        result.offset_ = packet_index * kBitsPerPacket + kBitsPerPacketHeader;
        return result;
      }
      continue;
    }

    const uint8_t* packet =
        memory()->TranslatePhysical(data->GetInputBufferAddress(buffer_index)) +
        packet_index * kBytesPerPacket;
    const uint32_t frame_offset = xma::GetPacketFrameOffset(packet);
    if (frame_offset <= kMaxFrameSizeinBits) {
      result.offset_ = packet_index * kBitsPerPacket +
                       (split_tail_bits ? kBitsPerPacketHeader + split_tail_bits
                                        : frame_offset);
      return result;
    }
    split_tail_bits = 0;

    // No frame starts in this packet, only the tail of one split across the
    // packet boundary. Follow its skip count to the stream's next packet.
    const uint8_t skip = xma::GetPacketSkipCount(packet);
    packet_index = skip == 0xFF ? packet_count : packet_index + skip + 1;
  }
}

const uint32_t XmaContextNew::GetAmountOfBitsToRead(
    const uint32_t remaining_stream_bits, const uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

uint32_t XmaContextNew::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContextNew::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

const kPacketInfo XmaContextNew::GetPacketInfo(uint8_t* packet,
                                               uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  auto consider_frame = [&](uint32_t offset, uint32_t size) {
    if (offset != frame_offset) {
      return;
    }
    packet_info.frame_found_ = true;
    packet_info.current_frame_ = packet_info.frame_count_;
    packet_info.current_frame_size_ = size;
  };

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      // This frame's 15-bit header runs into the next packet, so its size
      // isn't readable yet. Count it anyway, or the caller takes the previous
      // frame for the packet's last and skips straight past this one. Size 0
      // sends it to the split-header path.
      if (stream.BitsRemaining() > 0) {
        consider_frame(static_cast<uint32_t>(stream.offset_bits()), 0);
        packet_info.frame_count_++;
      }
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    consider_frame(static_cast<uint32_t>(stream.offset_bits()),
                   (uint32_t)frame_size);

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      // Last frame.
      break;
    }

    stream.Advance(frame_size - 1);

    // Read the trailing bit to see if frames follow
    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      // Frame header split across packet boundary — scanner couldn't
      // peek the full 15-bit header. Trust the XMA2 header count.
      if (packet_info.current_frame_size_ == 0) {
        // Current frame is the split-header frame
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    } else if (xma2_frame_count != packet_info.frame_count_) {
      XELOGE(
          "XmaContext {}: XMA2 packet header defines different amount of "
          "frames than internally found! (Header: {} Found: {})",
          id(), xma2_frame_count, packet_info.frame_count_);
    }
  }
  return packet_info;
}

int16_t XmaContextNew::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }

  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }

  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;

  return (int16_t)packet_number;
}

int XmaContextNew::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  // Re-initialize the context with new sample rate and channels.
  uint32_t channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->ch_layout.nb_channels != (int)channels) {
    XELOGAPU("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}",
             id(), av_context_->sample_rate, sample_rate,
             av_context_->ch_layout.nb_channels, channels);
    // We have to recreate the codec context so it'll realloc whatever data it
    // needs.
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

    av_context_->sample_rate = sample_rate;
    av_channel_layout_default(&av_context_->ch_layout, channels);
    av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      XELOGE("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

void XmaContextNew::PreparePacket(const uint32_t frame_size,
                                  const uint32_t frame_padding,
                                  const bool cut_frame) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size =
      static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                       (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2) |
                  (cut_frame ? 1 : 0);
}

kPacketResult XmaContextNew::DecodePacket(AVCodecContext* av_context,
                                          const AVPacket* av_packet,
                                          AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    XELOGE("XmaContext {}: Error sending packet for decoding: {} ({})", id(),
           errbuf, ret);
    return kPacketResult::kError;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    // Codec needs more input before producing output (e.g. first frame warmup).
    return kPacketResult::kNoFrame;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    XELOGE("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return kPacketResult::kError;
  }
  return kPacketResult::kFrame;
}

void XmaContextNew::SetError(XMA_CONTEXT_DATA* data, uint32_t status) {
  data->error_status = status;
  data->error_set = 1;
}

void XmaContextNew::StopOnParserError(XMA_CONTEXT_DATA* data, uint32_t status) {
  data->parser_error_status = status;
  data->parser_error_set = 1;
  data->error_status = 0;
  data->error_set = 0;
  decode_stop_ = kDecodeStop::kParserError;
}

void XmaContextNew::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                       const XMA_CONTEXT_DATA& initial_data,
                                       uint8_t* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  // DWORD 0: decoder owns loop_count, output_buffer_write_offset.
  // Only clear valid flags the decoder actually consumed (was 1, now 0).
  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  // DWORD 1: decoder owns decode_subframe, conditionally clears
  // output_buffer_valid
  fresh.decode_subframe = data.decode_subframe;
  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  // DWORD 2: decoder owns input_buffer_read_offset, error_status
  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;
  fresh.error_set = data.error_set;

  // DWORD 3: decoder owns parser_error_status
  fresh.parser_error_status = data.parser_error_status;
  fresh.parser_error_set = data.parser_error_set;

  // DWORD 4: decoder owns current_buffer
  fresh.current_buffer = data.current_buffer;

  // DWORD 9: decoder owns output_buffer_read_offset (reset by ClearLocked)
  fresh.output_buffer_read_offset = data.output_buffer_read_offset;

  fresh.Store(context_ptr);
}

}  // namespace apu
}  // namespace xe
