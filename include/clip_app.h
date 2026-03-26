#pragma once

#include <atomic>
#include <optional>
#include <thread>

#include "capture_manager.h"
#include "encoder_backend.h"
#include "replay_ring_buffer.h"

namespace klip {

class ClipApp {
 public:
  ClipApp();
  ~ClipApp();

  bool Initialize();
  void Run();
  void Stop();

 private:
  void EncodeThreadLoop();

  std::atomic<bool> running_{false};

  CaptureManager::OutputQueue captured_queue_;
  CaptureManager capture_manager_;
  ReplayRingBuffer ring_buffer_{16384, AVRational{1, 10000000}};
  EncoderBackend encoder_backend_;
  std::optional<CapturedFrame> bootstrap_frame_;

  std::thread encode_thread_;
};

}  // namespace klip
