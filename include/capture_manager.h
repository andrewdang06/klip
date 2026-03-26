#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include "lockfree_queues.h"

namespace klip {

struct CapturedFrame {
  winrt::com_ptr<ID3D11Texture2D> nv12_texture;
  int64_t pts_100ns = 0;
  uint64_t fence_value = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

class CaptureManager {
 public:
  using OutputQueue = SpscRingQueue<CapturedFrame, 512>;

  CaptureManager();
  ~CaptureManager();

  bool Initialize(OutputQueue* output_queue, uint32_t target_fps);
  bool Start();
  void Stop();

  ID3D11Device* device() const { return device_.get(); }
  ID3D11DeviceContext* context() const { return immediate_context_.get(); }
  ID3D11DeviceContext4* context4() const { return context4_.get(); }
  ID3D11Fence* fence() const { return fence_.get(); }
  uint32_t adapter_vendor_id() const { return adapter_vendor_id_; }
  uint32_t target_fps() const { return target_fps_; }

 private:
  struct RawFrame {
    winrt::com_ptr<ID3D11Texture2D> bgra_texture;
    int64_t pts_100ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  enum class TargetKind { kWindow, kMonitor };

  struct CaptureTarget {
    TargetKind kind = TargetKind::kMonitor;
    HWND hwnd = nullptr;
    HMONITOR hmonitor = nullptr;

    bool operator==(const CaptureTarget& other) const {
      return kind == other.kind && hwnd == other.hwnd && hmonitor == other.hmonitor;
    }

    bool operator!=(const CaptureTarget& other) const { return !(*this == other); }
  };

  bool CreateD3D11Device();
  bool CreateWgcDevice();
  bool CreateFence();
  bool StartCaptureForTarget(const CaptureTarget& target);
  void StopCaptureSessionLocked();

  CaptureTarget DeterminePreferredTarget() const;
  bool IsWindowCandidate(HWND hwnd) const;
  void AutoSwitchLoop();
  void CaptureProcessingLoop();

  void HandleFrameArrived(
      winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
      winrt::Windows::Foundation::IInspectable const&);

  bool EnsureVideoProcessor(uint32_t width, uint32_t height);
  bool RecreateNv12Pool(uint32_t width, uint32_t height);
  bool ConvertFrameToNv12(const RawFrame& in, CapturedFrame& out);
  winrt::com_ptr<ID3D11Texture2D> AcquireTextureFromSurface(
      const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) const;

  static winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateItemForWindow(HWND hwnd);
  static winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateItemForMonitor(HMONITOR monitor);

  static constexpr uint32_t kFramePoolSize = 4;
  static constexpr uint32_t kNv12PoolSize = 8;

  OutputQueue* output_queue_ = nullptr;
  uint32_t target_fps_ = 60;
  uint32_t adapter_vendor_id_ = 0;

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> fence_counter_{0};

  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> immediate_context_;
  winrt::com_ptr<ID3D11DeviceContext4> context4_;
  winrt::com_ptr<ID3D11VideoDevice> video_device_;
  winrt::com_ptr<ID3D11VideoContext> video_context_;
  winrt::com_ptr<ID3D11Device5> device5_;
  winrt::com_ptr<ID3D11Fence> fence_;

  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice wgc_device_{nullptr};

  std::mutex session_mutex_;
  CaptureTarget active_target_{};
  std::chrono::steady_clock::time_point pending_since_{};
  CaptureTarget pending_target_{};

  winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
  winrt::event_token frame_arrived_token_{};

  uint32_t current_width_ = 0;
  uint32_t current_height_ = 0;

  winrt::com_ptr<ID3D11VideoProcessorEnumerator> vp_enumerator_;
  winrt::com_ptr<ID3D11VideoProcessor> video_processor_;
  uint32_t vp_width_ = 0;
  uint32_t vp_height_ = 0;
  std::vector<winrt::com_ptr<ID3D11Texture2D>> nv12_pool_;
  std::atomic<uint32_t> nv12_pool_cursor_{0};

  SpscRingQueue<RawFrame, 512> raw_queue_;

  std::thread processing_thread_;
  std::thread auto_switch_thread_;
};

}  // namespace klip
