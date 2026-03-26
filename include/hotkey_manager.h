#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include <Windows.h>

namespace klip {

class HotkeyManager {
 public:
  using ClipCallback = std::function<void()>;

  HotkeyManager() = default;
  ~HotkeyManager();

  HotkeyManager(const HotkeyManager&) = delete;
  HotkeyManager& operator=(const HotkeyManager&) = delete;

  bool Start(ClipCallback on_clip);
  void Stop();

 private:
  void MessageLoop();

  std::atomic<bool> running_{false};
  std::thread thread_;
  DWORD thread_id_ = 0;
  ClipCallback on_clip_;
};

}  // namespace klip