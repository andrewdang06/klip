#include "hotkey_manager.h"

#include <chrono>
#include <utility>

namespace klip {

HotkeyManager::~HotkeyManager() {
  Stop();
}

bool HotkeyManager::Start(ClipCallback on_clip) {
  if (running_.exchange(true)) {
    return true;
  }

  on_clip_ = std::move(on_clip);
  thread_ = std::thread(&HotkeyManager::MessageLoop, this);

  // Wait until thread id is initialized.
  while (running_.load(std::memory_order_acquire) && thread_id_ == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return thread_id_ != 0;
}

void HotkeyManager::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (thread_id_ != 0) {
    PostThreadMessage(thread_id_, WM_QUIT, 0, 0);
  }

  if (thread_.joinable()) {
    thread_.join();
  }

  thread_id_ = 0;
}

void HotkeyManager::MessageLoop() {
  thread_id_ = GetCurrentThreadId();

  constexpr int kHotkeyClipId = 1;
  RegisterHotKey(nullptr, kHotkeyClipId, MOD_ALT | MOD_NOREPEAT, 'C');

  MSG msg{};
  while (running_.load(std::memory_order_acquire) && GetMessage(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyClipId) {
      if (on_clip_) {
        on_clip_();
      }
    }
  }

  UnregisterHotKey(nullptr, kHotkeyClipId);
}

}  // namespace klip
