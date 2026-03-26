#include "clip_app.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

#include "async_mp4_writer.h"
#include "hotkey_manager.h"

namespace klip {

namespace {

std::atomic<bool>* g_running_flag = nullptr;

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
  switch (signal) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      if (g_running_flag != nullptr) {
        g_running_flag->store(false, std::memory_order_release);
      }
      return TRUE;
    default:
      return FALSE;
  }
}

}  // namespace

ClipApp::ClipApp() = default;

ClipApp::~ClipApp() {
  Stop();
}

bool ClipApp::Initialize() {
  if (!capture_manager_.Initialize(&captured_queue_, 60)) {
    return false;
  }

  if (!capture_manager_.Start()) {
    return false;
  }

  using namespace std::chrono_literals;

  CapturedFrame first_frame;
  bool got_frame = false;
  const auto deadline = std::chrono::steady_clock::now() + 5s;

  while (std::chrono::steady_clock::now() < deadline) {
    if (captured_queue_.try_pop(first_frame)) {
      got_frame = true;
      break;
    }

    std::this_thread::sleep_for(1ms);
  }

  if (!got_frame) {
    capture_manager_.Stop();
    return false;
  }

  if (!encoder_backend_.Initialize(
          capture_manager_.device(),
          capture_manager_.context(),
          capture_manager_.adapter_vendor_id(),
          first_frame.width,
          first_frame.height,
          capture_manager_.target_fps(),
          &ring_buffer_)) {
    capture_manager_.Stop();
    return false;
  }

  bootstrap_frame_ = std::move(first_frame);

  running_.store(true, std::memory_order_release);
  encode_thread_ = std::thread(&ClipApp::EncodeThreadLoop, this);

  return true;
}

void ClipApp::Run() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  const std::filesystem::path clips_dir = std::filesystem::current_path() / "clips";
  std::error_code dir_ec{};
  std::filesystem::create_directories(clips_dir, dir_ec);
  std::cout << "Clip output folder: " << clips_dir.string() << std::endl;

  AsyncMp4Writer writer;
  writer.Start(&ring_buffer_, &encoder_backend_);

  HotkeyManager hotkey;
  hotkey.Start([&writer]() {
    if (writer.RequestWrite()) {
      std::cout << "Clip requested (Alt+C)." << std::endl;
    } else {
      std::cout << "Clip request failed (writer queue full)." << std::endl;
    }
  });

  g_running_flag = &running_;
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  using namespace std::chrono_literals;
  auto last_auto_request = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() - last_auto_request >= 5s) {
      last_auto_request = std::chrono::steady_clock::now();
      if (writer.RequestWrite()) {
        std::cout << "Automatic clip requested." << std::endl;
      } else {
        std::cout << "Automatic clip request failed (writer queue full)." << std::endl;
      }
    }

    std::this_thread::sleep_for(200ms);
  }

  SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
  g_running_flag = nullptr;

  hotkey.Stop();
  writer.Stop();
  Stop();
}

void ClipApp::Stop() {
  running_.store(false, std::memory_order_release);

  if (encode_thread_.joinable()) {
    encode_thread_.join();
  }

  encoder_backend_.Flush();
  capture_manager_.Stop();
}

void ClipApp::EncodeThreadLoop() {
  using namespace std::chrono_literals;

  HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_event == nullptr) {
    return;
  }

  auto process_frame = [&](const CapturedFrame& frame) {
    if (capture_manager_.fence() == nullptr) {
      return;
    }

    if (FAILED(capture_manager_.fence()->SetEventOnCompletion(frame.fence_value, fence_event))) {
      return;
    }

    if (WaitForSingleObject(fence_event, 1000) != WAIT_OBJECT_0) {
      return;
    }

    encoder_backend_.EncodeTexture(frame.nv12_texture.get(), frame.pts_100ns);
  };

  if (bootstrap_frame_.has_value()) {
    process_frame(bootstrap_frame_.value());
    bootstrap_frame_.reset();
  }

  while (running_.load(std::memory_order_acquire)) {
    CapturedFrame frame;
    if (!captured_queue_.try_pop(frame)) {
      std::this_thread::sleep_for(1ms);
      continue;
    }

    process_frame(frame);
  }

  CloseHandle(fence_event);
}

}  // namespace klip