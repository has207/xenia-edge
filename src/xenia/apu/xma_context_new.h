/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_XMA_CONTEXT_NEW_H_
#define XENIA_APU_XMA_CONTEXT_NEW_H_

#include <array>
#include <atomic>
#include <mutex>
#include <queue>

#include "xenia/apu/xma_context.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

// Forward declarations
struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace xe {
namespace apu {

struct kPacketInfo {
  uint8_t frame_count_;
  uint8_t current_frame_;
  uint32_t current_frame_size_;
  // A frame starts at the requested offset.
  bool frame_found_;

  const bool isLastFrameInPacket() const {
    return frame_count_ == 0 || current_frame_ == frame_count_ - 1;
  }
};

struct kPacketHandle {
  uint32_t buffer_index_ = 0;
  uint32_t packet_index_ = 0;
  bool is_valid_ = false;
};

// Why Decode() stopped without a frame to output.
enum class kDecodeStop {
  kNone,
  // The frame continues in the other input buffer, which isn't valid yet.
  kWaitForBuffer,
  // The frame failed to decode, or no frame starts at the position.
  kParserError,
};

enum class kPacketResult { kFrame, kNoFrame, kError };

// Where decoding goes once the frame being output is done.
struct kPendingAdvance {
  bool pending_ = false;
  bool loop_jump_ = false;
  uint32_t offset_ = 0;
  uint32_t buffers_consumed_ = 0;
};

struct kNextFrame {
  // Bit offset in the buffer the frame is in.
  uint32_t offset_ = 0;
  // Input buffers finished on the way: the current one, then the other one
  // when the stream has no frame start left in it either.
  uint32_t buffers_consumed_ = 0;
};

static constexpr int kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

class XmaContextNew : public XmaContext {
 public:
  static constexpr uint32_t kBitsPerPacketHeader = 32;
  static constexpr uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;

  // DWORD2 error_status: why decoding last stopped.
  static constexpr uint32_t kErrorOutputFull = 1;
  static constexpr uint32_t kErrorOutputSpace = 2;
  static constexpr uint32_t kErrorNoInput = 4;
  static constexpr uint32_t kErrorWaitForBuffer = 8;
  // DWORD3 parser_error_status, until a frame decodes.
  static constexpr uint32_t kParserErrorBitstream = 3;
  static constexpr uint32_t kParserErrorNoFrame = 4;

  explicit XmaContextNew();
  ~XmaContextNew();

  int Setup(uint32_t id, Memory* memory, uint32_t guest_ptr);
  bool Work();

  void Enable();
  void Clear();
  void Disable();
  void Release();

 private:
  void ClearLocked(XMA_CONTEXT_DATA* data);
  void FlushDecoder();
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  // Convert sampling rate from ID to frequency.
  static int GetSampleRate(int id);
  // Get the containing packet number of the frame pointed to by the offset.
  static int16_t GetPacketNumber(size_t size, size_t bit_offset);

  const kPacketInfo GetPacketInfo(uint8_t* packet, uint32_t frame_offset);

  const uint32_t GetAmountOfBitsToRead(const uint32_t remaining_stream_bits,
                                       const uint32_t frame_size);

  kPacketHandle GetPacketHandle(XMA_CONTEXT_DATA* data, uint32_t buffer_index,
                                uint32_t packet_index,
                                uint32_t current_input_packet_count);

  const uint8_t* GetNextPacket(XMA_CONTEXT_DATA* data,
                               uint32_t next_packet_index,
                               uint32_t current_input_packet_count);

  // The stream's next frame start from packet next_packet_index of the
  // current buffer on, following packet skip counts. Indices past the end of
  // the current buffer continue in the other one, so a multi-stream context
  // keeps its stream's packet across the boundary. When that buffer isn't
  // valid yet the offset is the start of the stream's packet in it. Each
  // consumed buffer is swapped out by the caller. A frame split into the
  // first packet by split_tail_bits is followed by the frame at its end, if
  // that packet holds a frame start.
  kNextFrame FindNextFrame(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                           uint32_t current_input_packet_count,
                           uint32_t split_tail_bits = 0);

  // Returns currently used buffer
  uint8_t* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data);

  static uint32_t GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);

  void Decode(XMA_CONTEXT_DATA* data);
  void Consume(RingBuffer* XE_RESTRICT output_rb,
               const XMA_CONTEXT_DATA* const XE_RESTRICT data);

  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  // The frame at the position is cut after loop_subframe_end and jumps to
  // loop_start, unless decoding starts past loop_subframe_end.
  static bool IsLoopEndFrame(const XMA_CONTEXT_DATA* data);
  // Room the next pass needs and the frames it covers: the rest of the frame
  // and the next frame if they fit in subframe_decode_count subframes, else
  // that many subframes of the frame. A loop end cut doesn't shrink it.
  uint32_t GetPassBlocks(const XMA_CONTEXT_DATA* data,
                         uint32_t* frames = nullptr) const;
  // Moves decoding on to the next frame, or loop_start for a loop jump, once
  // the frame being output is done. Until then the position stays inside it.
  void AdvanceAfterFrame(XMA_CONTEXT_DATA* data, uint32_t offset,
                         uint32_t buffers_consumed, bool loop_jump);
  void ApplyAdvance(XMA_CONTEXT_DATA* data);
  // Leaves the read offset and decode_subframe at the position inside a frame
  // that is part way out, and keeps them to tell a later seek.
  void WritePosition(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(int sample_rate, bool is_two_channel);
  // A cut frame stops before its last subframe, so the next frame overlaps
  // the one before it.
  void PreparePacket(const uint32_t frame_size, const uint32_t frame_padding,
                     const bool cut_frame);

  RingBuffer PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data);

  kPacketResult DecodePacket(AVCodecContext* av_context,
                             const AVPacket* av_packet, AVFrame* av_frame);
  static void SetError(XMA_CONTEXT_DATA* data, uint32_t status);
  // Sets the parser status, clears the error status and stops decoding with
  // the position left where it is.
  void StopOnParserError(XMA_CONTEXT_DATA* data, uint32_t status);

  // Re-reads context from guest memory and merges only decoder-owned fields,
  // preserving any game modifications made during decoding.
  void StoreContextMerged(const XMA_CONTEXT_DATA& data,
                          const XMA_CONTEXT_DATA& initial_data,
                          uint8_t* context_ptr);

  std::array<uint8_t, kBytesPerPacketData * 2> input_buffer_;
  // first byte contains bit offset information
  std::array<uint8_t, 1 + 4096> xma_frame_;
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_;

  int32_t remaining_subframe_blocks_in_output_buffer_ = 0;
  uint8_t current_frame_remaining_subframes_ = 0;
  // The block after the last one the frame being output writes: the end of
  // the frame, or of loop_subframe_end on a loop end frame.
  uint8_t current_frame_end_block_ = 0;

  kPendingAdvance advance_;
  kDecodeStop decode_stop_ = kDecodeStop::kNone;
  // The position WritePosition() left in the context.
  uint32_t held_offset_ = 0;
  uint32_t held_subframe_ = 0;
  uint32_t held_buffer_ = 0;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_XMA_CONTEXT_H_
