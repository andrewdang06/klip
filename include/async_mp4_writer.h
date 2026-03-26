#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "encoder_backend.h"
#include "lockfree_queues.h"
#include "replay_ring_buffer.h"

namespace klip {

class AsyncMp4Writer {
 public:
  AsyncMp4Writer();
  ~AsyncMp4Writer();

  AsyncMp4Writer(const AsyncMp4Writer&) = delete;
  AsyncMp4Writer& operator=(const AsyncMp4Writer&) = delete;

  bool Start(const ReplayRingBuffer* ring_buffer, const EncoderBackend* encoder);
  void Stop();

  bool RequestWrite();

 private:
  struct Request {
    std::wstring output_path;
    double window_seconds = 60.0;
  };

  static std::wstring BuildOutputPath();
  static bool WriteMp4File(const std::wstring& path,
                           const std::vector<AvPacketPtr>& packets,
                           const AVCodecContext* codec_ctx,
                           AVRational packet_time_base);

  void ThreadLoop();

  const ReplayRingBuffer* ring_buffer_ = nullptr;
  const EncoderBackend* encoder_ = nullptr;

  std::atomic<bool> running_{false};
  MpmcBoundedQueue<Request, 64> queue_;
  std::thread thread_;
};

}  // namespace klip