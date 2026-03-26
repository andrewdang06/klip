#pragma once

#include <bit>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

namespace klip {

struct AvPacketDeleter {
  void operator()(AVPacket* packet) const {
    if (packet != nullptr) {
      av_packet_free(&packet);
    }
  }
};

using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;

class ReplayRingBuffer {
 public:
  ReplayRingBuffer(std::size_t requested_capacity, AVRational time_base);
  ~ReplayRingBuffer();

  ReplayRingBuffer(const ReplayRingBuffer&) = delete;
  ReplayRingBuffer& operator=(const ReplayRingBuffer&) = delete;

  bool Push(AVPacket* packet);
  std::vector<AvPacketPtr> SnapshotLastWindow(double seconds) const;

  AVRational time_base() const { return time_base_; }

 private:
  struct Slot {
    AvPacketPtr packet;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    int64_t duration = 0;
    int flags = 0;
    std::atomic<uint64_t> stamp{std::numeric_limits<uint64_t>::max()};
  };

  struct PacketMeta {
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    int64_t duration = 0;
    int flags = 0;
  };

  static bool IsPowerOfTwo(std::size_t v) { return v != 0 && (v & (v - 1)) == 0; }

  std::optional<PacketMeta> ReadMeta(uint64_t seq) const;
  bool ReadPacketRef(uint64_t seq, AVPacket* out) const;
  uint64_t LatestSequence() const;

  std::unique_ptr<Slot[]> slots_;
  std::size_t capacity_ = 0;
  std::size_t mask_ = 0;
  AVRational time_base_{};
  std::atomic<uint64_t> write_seq_{0};
};

}  // namespace klip
