#include "replay_ring_buffer.h"

#include <algorithm>

namespace klip {

namespace {

int64_t SecondsToPts(double seconds, AVRational time_base) {
  const int64_t microseconds = static_cast<int64_t>(seconds * 1'000'000.0);
  return av_rescale_q(microseconds, AVRational{1, 1'000'000}, time_base);
}

}  // namespace

ReplayRingBuffer::ReplayRingBuffer(std::size_t requested_capacity, AVRational time_base)
    : capacity_(std::bit_ceil(std::max<std::size_t>(requested_capacity, 2))),
      mask_(capacity_ - 1),
      time_base_(time_base) {
  if (!IsPowerOfTwo(capacity_)) {
    capacity_ = std::bit_ceil(capacity_);
    mask_ = capacity_ - 1;
  }

  slots_ = std::make_unique<Slot[]>(capacity_);
  for (std::size_t i = 0; i < capacity_; ++i) {
    slots_[i].packet.reset(av_packet_alloc());
  }
}

ReplayRingBuffer::~ReplayRingBuffer() = default;

bool ReplayRingBuffer::Push(AVPacket* packet) {
  if (packet == nullptr) {
    return false;
  }

  const uint64_t seq = write_seq_.fetch_add(1, std::memory_order_relaxed);
  Slot& slot = slots_[static_cast<std::size_t>(seq & mask_)];

  slot.stamp.store((seq << 1) | 1ULL, std::memory_order_release);

  av_packet_unref(slot.packet.get());
  av_packet_move_ref(slot.packet.get(), packet);

  slot.pts = slot.packet->pts;
  slot.dts = slot.packet->dts;
  slot.duration = slot.packet->duration;
  slot.flags = slot.packet->flags;

  slot.stamp.store(seq << 1, std::memory_order_release);
  return true;
}

std::optional<ReplayRingBuffer::PacketMeta> ReplayRingBuffer::ReadMeta(uint64_t seq) const {
  const Slot& slot = slots_[static_cast<std::size_t>(seq & mask_)];
  const uint64_t expected = seq << 1;

  const uint64_t s1 = slot.stamp.load(std::memory_order_acquire);
  if (s1 != expected) {
    return std::nullopt;
  }

  PacketMeta meta{slot.pts, slot.dts, slot.duration, slot.flags};

  const uint64_t s2 = slot.stamp.load(std::memory_order_acquire);
  if (s2 != expected) {
    return std::nullopt;
  }

  return meta;
}

bool ReplayRingBuffer::ReadPacketRef(uint64_t seq, AVPacket* out) const {
  Slot& slot = const_cast<Slot&>(slots_[static_cast<std::size_t>(seq & mask_)]);
  const uint64_t expected = seq << 1;

  for (int attempt = 0; attempt < 3; ++attempt) {
    const uint64_t s1 = slot.stamp.load(std::memory_order_acquire);
    if (s1 != expected) {
      return false;
    }

    if (av_packet_ref(out, slot.packet.get()) < 0) {
      return false;
    }

    const uint64_t s2 = slot.stamp.load(std::memory_order_acquire);
    if (s2 == expected) {
      return true;
    }

    av_packet_unref(out);
  }

  return false;
}

uint64_t ReplayRingBuffer::LatestSequence() const {
  return write_seq_.load(std::memory_order_acquire);
}

std::vector<AvPacketPtr> ReplayRingBuffer::SnapshotLastWindow(double seconds) const {
  std::vector<AvPacketPtr> result;

  const uint64_t end_seq = LatestSequence();
  if (end_seq == 0) {
    return result;
  }

  const uint64_t start_available = (end_seq > capacity_) ? (end_seq - capacity_) : 0;

  std::optional<PacketMeta> latest;
  uint64_t latest_seq = 0;
  for (uint64_t seq = end_seq; seq > start_available; --seq) {
    auto meta = ReadMeta(seq - 1);
    if (meta.has_value()) {
      latest = meta;
      latest_seq = seq - 1;
      break;
    }
  }

  if (!latest.has_value()) {
    return result;
  }

  int64_t threshold_pts = std::numeric_limits<int64_t>::min();
  if (latest->pts != AV_NOPTS_VALUE) {
    threshold_pts = latest->pts - SecondsToPts(seconds, time_base_);
  }

  uint64_t candidate_seq = start_available;
  bool candidate_found = false;
  uint64_t prior_key_seq = std::numeric_limits<uint64_t>::max();

  for (uint64_t seq = start_available; seq <= latest_seq; ++seq) {
    auto meta = ReadMeta(seq);
    if (!meta.has_value()) {
      continue;
    }

    if ((meta->flags & AV_PKT_FLAG_KEY) != 0) {
      if (!candidate_found && (meta->pts == AV_NOPTS_VALUE || meta->pts <= threshold_pts)) {
        prior_key_seq = seq;
      }
    }

    if (!candidate_found && meta->pts != AV_NOPTS_VALUE && meta->pts >= threshold_pts) {
      candidate_seq = seq;
      candidate_found = true;
      break;
    }
  }

  if (!candidate_found) {
    candidate_seq = start_available;
  }

  uint64_t start_seq = candidate_seq;
  if (prior_key_seq != std::numeric_limits<uint64_t>::max()) {
    start_seq = prior_key_seq;
  } else {
    for (uint64_t seq = candidate_seq; seq <= latest_seq; ++seq) {
      auto meta = ReadMeta(seq);
      if (meta.has_value() && ((meta->flags & AV_PKT_FLAG_KEY) != 0)) {
        start_seq = seq;
        break;
      }
    }
  }

  for (uint64_t seq = start_seq; seq <= latest_seq; ++seq) {
    auto packet = AvPacketPtr(av_packet_alloc());
    if (!packet) {
      continue;
    }

    if (!ReadPacketRef(seq, packet.get())) {
      continue;
    }

    result.emplace_back(std::move(packet));
  }

  return result;
}

}  // namespace klip
