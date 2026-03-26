#include "capture_manager.h"

#include <Windows.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <array>
#include <chrono>
#include <thread>

#include <winrt/base.h>

namespace klip {

namespace {

int64_t QueryQpc100ns() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return (counter.QuadPart * 10'000'000LL) / frequency.QuadPart;
}

bool IsWindowCloaked(HWND hwnd) {
  DWORD cloaked = 0;
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
    return false;
  }
  return cloaked != 0;
}

}  // namespace

CaptureManager::CaptureManager() = default;

CaptureManager::~CaptureManager() {
  Stop();
}

bool CaptureManager::Initialize(OutputQueue* output_queue, uint32_t target_fps) {
  output_queue_ = output_queue;
  target_fps_ = target_fps;

  if (output_queue_ == nullptr) {
    return false;
  }

  if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
    return false;
  }

  if (!CreateD3D11Device()) {
    return false;
  }

  if (!CreateWgcDevice()) {
    return false;
  }

  if (!CreateFence()) {
    return false;
  }

  pending_target_ = CaptureTarget{TargetKind::kMonitor, nullptr, MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)};
  active_target_ = pending_target_;
  pending_since_ = std::chrono::steady_clock::now();
  return true;
}

bool CaptureManager::Start() {
  if (running_.exchange(true)) {
    return true;
  }

  active_target_ = DeterminePreferredTarget();
  pending_target_ = active_target_;
  pending_since_ = std::chrono::steady_clock::now();

  {
    std::scoped_lock lock(session_mutex_);
    if (!StartCaptureForTarget(active_target_)) {
      const CaptureTarget fallback{
          TargetKind::kMonitor, nullptr, MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)};
      if (!StartCaptureForTarget(fallback)) {
        running_.store(false);
        return false;
      }
      active_target_ = fallback;
      pending_target_ = fallback;
    }
  }

  processing_thread_ = std::thread(&CaptureManager::CaptureProcessingLoop, this);
  auto_switch_thread_ = std::thread(&CaptureManager::AutoSwitchLoop, this);

  return true;
}

void CaptureManager::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  {
    std::scoped_lock lock(session_mutex_);
    StopCaptureSessionLocked();
  }

  if (auto_switch_thread_.joinable()) {
    auto_switch_thread_.join();
  }

  if (processing_thread_.joinable()) {
    processing_thread_.join();
  }
}

bool CaptureManager::CreateD3D11Device() {
  constexpr UINT kFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  constexpr D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };

  D3D_FEATURE_LEVEL feature_level{};
  HRESULT hr = D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      kFlags,
      levels,
      static_cast<UINT>(std::size(levels)),
      D3D11_SDK_VERSION,
      device_.put(),
      &feature_level,
      immediate_context_.put());

  if (FAILED(hr)) {
    return false;
  }

  if (FAILED(immediate_context_->QueryInterface(IID_PPV_ARGS(context4_.put())))) {
    return false;
  }

  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(video_device_.put())))) {
    return false;
  }

  if (FAILED(immediate_context_->QueryInterface(IID_PPV_ARGS(video_context_.put())))) {
    return false;
  }

  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(device5_.put())))) {
    return false;
  }

  winrt::com_ptr<IDXGIDevice> dxgi_device;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(dxgi_device.put())))) {
    return false;
  }

  winrt::com_ptr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(adapter.put()))) {
    return false;
  }

  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) {
    return false;
  }

  adapter_vendor_id_ = desc.VendorId;
  return true;
}

bool CaptureManager::CreateWgcDevice() {
  winrt::com_ptr<IDXGIDevice> dxgi_device;
  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(dxgi_device.put())))) {
    return false;
  }

  winrt::com_ptr<::IInspectable> inspectable;
  if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()))) {
    return false;
  }

  wgc_device_ = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
  return true;
}

bool CaptureManager::CreateFence() {
  return SUCCEEDED(device5_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.put())));
}

CaptureManager::CaptureTarget CaptureManager::DeterminePreferredTarget() const {
  const HWND hwnd = GetForegroundWindow();
  if (IsWindowCandidate(hwnd)) {
    return CaptureTarget{TargetKind::kWindow, hwnd, nullptr};
  }

  return CaptureTarget{TargetKind::kMonitor, nullptr, MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)};
}

bool CaptureManager::IsWindowCandidate(HWND hwnd) const {
  if (hwnd == nullptr || hwnd == GetConsoleWindow()) {
    return false;
  }

  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
    return false;
  }

  if (IsWindowCloaked(hwnd)) {
    return false;
  }

  return true;
}

void CaptureManager::AutoSwitchLoop() {
  using namespace std::chrono_literals;

  while (running_.load(std::memory_order_acquire)) {
    const CaptureTarget preferred = DeterminePreferredTarget();
    const auto now = std::chrono::steady_clock::now();

    if (preferred != pending_target_) {
      pending_target_ = preferred;
      pending_since_ = now;
    } else if (preferred != active_target_) {
      if ((now - pending_since_) >= 500ms) {
        std::scoped_lock lock(session_mutex_);
        if (running_.load(std::memory_order_relaxed) && preferred != active_target_) {
          if (StartCaptureForTarget(preferred)) {
            active_target_ = preferred;
          }
        }
      }
    }

    std::this_thread::sleep_for(100ms);
  }
}

bool CaptureManager::StartCaptureForTarget(const CaptureTarget& target) {
  StopCaptureSessionLocked();

  try {
    item_ = (target.kind == TargetKind::kWindow) ? CreateItemForWindow(target.hwnd)
                                                  : CreateItemForMonitor(target.hmonitor);
  } catch (...) {
    item_ = nullptr;
    return false;
  }

  if (item_ == nullptr) {
    return false;
  }

  const auto size = item_.Size();
  if (size.Width <= 0 || size.Height <= 0) {
    return false;
  }

  current_width_ = static_cast<uint32_t>(size.Width);
  current_height_ = static_cast<uint32_t>(size.Height);

  frame_pool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
      wgc_device_,
      winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
      kFramePoolSize,
      size);

  frame_arrived_token_ = frame_pool_.FrameArrived(
      {this, &CaptureManager::HandleFrameArrived});

  session_ = frame_pool_.CreateCaptureSession(item_);
  session_.StartCapture();

  active_target_ = target;
  EnsureVideoProcessor(current_width_, current_height_);
  return true;
}

void CaptureManager::StopCaptureSessionLocked() {
  if (frame_pool_ != nullptr) {
    try {
      frame_pool_.FrameArrived(frame_arrived_token_);
    } catch (...) {
    }
    frame_arrived_token_ = {};
  }

  if (session_ != nullptr) {
    session_.Close();
    session_ = nullptr;
  }

  if (frame_pool_ != nullptr) {
    frame_pool_.Close();
    frame_pool_ = nullptr;
  }

  item_ = nullptr;
}

void CaptureManager::HandleFrameArrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const&) {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
  {
    std::scoped_lock lock(session_mutex_);
    if (!running_.load(std::memory_order_relaxed) || sender == nullptr) {
      return;
    }

    frame = sender.TryGetNextFrame();
    if (frame == nullptr) {
      return;
    }

    const auto size = frame.ContentSize();
    if (size.Width <= 0 || size.Height <= 0) {
      return;
    }

    const uint32_t width = static_cast<uint32_t>(size.Width);
    const uint32_t height = static_cast<uint32_t>(size.Height);

    if (width != current_width_ || height != current_height_) {
      current_width_ = width;
      current_height_ = height;

      sender.Recreate(
          wgc_device_,
          winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
          kFramePoolSize,
          size);

      EnsureVideoProcessor(width, height);
      return;
    }

    auto source_texture = AcquireTextureFromSurface(frame.Surface());
    if (source_texture == nullptr) {
      return;
    }

    RawFrame raw{};
    raw.bgra_texture = std::move(source_texture);
    raw.width = width;
    raw.height = height;

    const auto time = frame.SystemRelativeTime();
    raw.pts_100ns = (time.count() != 0) ? time.count() : QueryQpc100ns();

    if (!raw_queue_.try_push(std::move(raw))) {
      // Queue full: drop frame rather than stalling capture callback.
    }
  }
}

void CaptureManager::CaptureProcessingLoop() {
  using namespace std::chrono_literals;

  while (running_.load(std::memory_order_acquire)) {
    RawFrame raw;
    if (!raw_queue_.try_pop(raw)) {
      std::this_thread::sleep_for(1ms);
      continue;
    }

    CapturedFrame converted;
    if (!ConvertFrameToNv12(raw, converted)) {
      continue;
    }

    if (!output_queue_->try_push(std::move(converted))) {
      // Downstream overloaded: drop oldest frame path by discarding this frame.
    }
  }
}

bool CaptureManager::EnsureVideoProcessor(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return false;
  }

  if (vp_enumerator_ != nullptr && video_processor_ != nullptr && width == vp_width_ &&
      height == vp_height_) {
    return true;
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc{};
  content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content_desc.InputWidth = width;
  content_desc.InputHeight = height;
  content_desc.OutputWidth = width;
  content_desc.OutputHeight = height;
  content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  vp_enumerator_ = nullptr;
  video_processor_ = nullptr;

  if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content_desc, vp_enumerator_.put()))) {
    return false;
  }

  if (FAILED(video_device_->CreateVideoProcessor(vp_enumerator_.get(), 0, video_processor_.put()))) {
    return false;
  }

  if (!RecreateNv12Pool(width, height)) {
    return false;
  }

  vp_width_ = width;
  vp_height_ = height;
  return true;
}

bool CaptureManager::RecreateNv12Pool(uint32_t width, uint32_t height) {
  nv12_pool_.clear();
  nv12_pool_.reserve(kNv12PoolSize);

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
#ifdef D3D11_BIND_VIDEO_ENCODER
  desc.BindFlags |= D3D11_BIND_VIDEO_ENCODER;
#endif

  for (uint32_t i = 0; i < kNv12PoolSize; ++i) {
    winrt::com_ptr<ID3D11Texture2D> texture;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, texture.put()))) {
      nv12_pool_.clear();
      return false;
    }
    nv12_pool_.push_back(std::move(texture));
  }

  nv12_pool_cursor_.store(0, std::memory_order_relaxed);
  return true;
}

bool CaptureManager::ConvertFrameToNv12(const RawFrame& in, CapturedFrame& out) {
  if (!EnsureVideoProcessor(in.width, in.height)) {
    return false;
  }

  if (nv12_pool_.empty()) {
    return false;
  }

  const uint32_t idx = nv12_pool_cursor_.fetch_add(1, std::memory_order_relaxed) % nv12_pool_.size();
  auto output_texture = nv12_pool_[idx];

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
  in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  in_desc.Texture2D.ArraySlice = 0;

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
  out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  out_desc.Texture2D.MipSlice = 0;

  winrt::com_ptr<ID3D11VideoProcessorInputView> in_view;
  winrt::com_ptr<ID3D11VideoProcessorOutputView> out_view;

  if (FAILED(video_device_->CreateVideoProcessorInputView(
          in.bgra_texture.get(), vp_enumerator_.get(), &in_desc, in_view.put()))) {
    return false;
  }

  if (FAILED(video_device_->CreateVideoProcessorOutputView(
          output_texture.get(), vp_enumerator_.get(), &out_desc, out_view.put()))) {
    return false;
  }

  RECT rect{0, 0, static_cast<LONG>(in.width), static_cast<LONG>(in.height)};
  video_context_->VideoProcessorSetOutputTargetRect(video_processor_.get(), TRUE, &rect);
  video_context_->VideoProcessorSetStreamSourceRect(video_processor_.get(), 0, TRUE, &rect);
  video_context_->VideoProcessorSetStreamDestRect(video_processor_.get(), 0, TRUE, &rect);
  video_context_->VideoProcessorSetStreamFrameFormat(
      video_processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = in_view.get();

  if (FAILED(video_context_->VideoProcessorBlt(video_processor_.get(), out_view.get(), 0, 1, &stream))) {
    return false;
  }

  const uint64_t fence_value = fence_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (FAILED(context4_->Signal(fence_.get(), fence_value))) {
    return false;
  }

  out.nv12_texture = std::move(output_texture);
  out.pts_100ns = in.pts_100ns;
  out.fence_value = fence_value;
  out.width = in.width;
  out.height = in.height;
  return true;
}

winrt::com_ptr<ID3D11Texture2D> CaptureManager::AcquireTextureFromSurface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) const {
  auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  winrt::com_ptr<ID3D11Texture2D> texture;

  if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()))) {
    return nullptr;
  }

  return texture;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureManager::CreateItemForWindow(HWND hwnd) {
  auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                               IGraphicsCaptureItemInterop>();

  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
  winrt::check_hresult(
      interop->CreateForWindow(
          hwnd,
          __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
          winrt::put_abi(item)));

  return item;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureManager::CreateItemForMonitor(HMONITOR monitor) {
  auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                               IGraphicsCaptureItemInterop>();

  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
  winrt::check_hresult(
      interop->CreateForMonitor(
          monitor,
          __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
          winrt::put_abi(item)));

  return item;
}

}  // namespace klip
