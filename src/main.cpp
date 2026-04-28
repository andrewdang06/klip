#include <Windows.h>
#include <audioclient.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "lockfree_queues.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

namespace {

using klip::SpscRingQueue;

struct AvPacketDeleter {
  void operator()(AVPacket* packet) const {
    if (packet != nullptr) {
      av_packet_free(&packet);
    }
  }
};

using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;

std::string AvErrorToString(int error_code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  av_strerror(error_code, buffer.data(), buffer.size());
  return std::string(buffer.data());
}

enum class EngineStatus {
  kIdle,
  kBuffering,
  kSaving,
};

enum class TargetMode {
  kActiveWindow = 0,
  kFullDesktop = 1,
};

struct D3DState {
  winrt::com_ptr<ID3D11Device> device;
  winrt::com_ptr<ID3D11DeviceContext> context;
  winrt::com_ptr<IDXGISwapChain> swap_chain;
  winrt::com_ptr<ID3D11RenderTargetView> render_target_view;
  UINT pending_width = 0;
  UINT pending_height = 0;
};

struct CodecSnapshot {
  AVCodecParameters* params = nullptr;
  AVRational time_base{1, 10'000'000};

  CodecSnapshot() = default;
  CodecSnapshot(const CodecSnapshot&) = delete;
  CodecSnapshot& operator=(const CodecSnapshot&) = delete;
  CodecSnapshot(CodecSnapshot&& other) noexcept;
  CodecSnapshot& operator=(CodecSnapshot&& other) noexcept;
  ~CodecSnapshot();
};

enum class PacketKind {
  kVideo,
  kAudio,
};

struct BufferedPacket {
  AvPacketPtr packet;
  PacketKind kind = PacketKind::kVideo;
};

class PacketRingBuffer {
 public:
  PacketRingBuffer(std::size_t capacity, AVRational time_base);

  void Clear();
  bool Push(const AVPacket* packet, PacketKind kind);
  bool empty() const;
  std::size_t size() const;
  double BufferedSeconds() const;
  std::vector<BufferedPacket> SnapshotLastWindow(double seconds) const;
  AVRational time_base() const { return time_base_; }

 private:
  std::vector<BufferedPacket> CloneOrderedPackets() const;

  mutable std::mutex mutex_;
  std::vector<BufferedPacket> packets_;
  AVRational time_base_{1, 10'000'000};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
};

class WASAPIAudioCapture {
 public:
  struct MicrophoneInfo {
    std::wstring id;
    std::string display_name;
  };

  bool Initialize(PacketRingBuffer* ring_buffer, int64_t qpc_origin, int64_t qpc_frequency);
  void Shutdown();

  std::vector<MicrophoneInfo> microphones() const;
  int selected_microphone_index() const;
  void SetSelectedMicrophoneIndex(int index);

  bool game_audio_active() const;
  bool microphone_active() const;
  float game_audio_level() const;
  float microphone_level() const;
  bool CopyCodecSnapshot(CodecSnapshot& snapshot) const;

 private:
  enum class SourceKind {
    kLoopback,
    kMicrophone,
  };

  struct SourceBuffer {
    mutable std::mutex mutex;
    std::deque<float> samples;
    int64_t read_pts_100ns = AV_NOPTS_VALUE;
    std::atomic<int64_t> last_activity_100ns{0};
    std::atomic<float> peak_level{0.0f};
    std::atomic<int64_t> peak_pts_100ns{0};
  };

  struct CaptureContext {
    SourceKind kind = SourceKind::kLoopback;
    std::wstring device_id;
    winrt::com_ptr<IMMDevice> device;
    winrt::com_ptr<IAudioClient> audio_client;
    winrt::com_ptr<IAudioCaptureClient> capture_client;
    WAVEFORMATEX* mix_format = nullptr;
    SwrContext* swr = nullptr;
    HANDLE sample_ready_event = nullptr;
    std::vector<float> convert_buffer;
    std::thread thread;
    std::atomic<bool> running{false};
  };

  bool RefreshMicrophones();
  bool InitializeAudioEncoder();
  bool StartLoopbackCapture();
  bool StartMicrophoneCaptureLocked(int index);
  void StopCaptureContext(CaptureContext& context);
  void CaptureThreadMain(CaptureContext* context, SourceBuffer* buffer);
  bool OpenCaptureContext(CaptureContext& context, const std::wstring& device_id, DWORD stream_flags);
  static AVSampleFormat WaveFormatToAvSampleFormat(const WAVEFORMATEX* mix_format);
  static std::string WideToUtf8(const std::wstring& value);
  static int64_t SamplesTo100ns(int sample_count, int sample_rate);
  static int64_t FramesTo100ns(int frame_count, int sample_rate);
  static int PtsToFrames(int64_t pts_100ns, int sample_rate);
  float QueryLevel(const SourceBuffer& buffer) const;
  void AppendSourceSamples(SourceBuffer& buffer, int64_t pts_100ns, const float* samples, int frame_count);
  bool MixNextFrame(std::vector<float>& mixed_interleaved, int& mixed_frame_count, int64_t& pts_100ns);
  void MixerThreadMain();
  bool EncodeMixedFrame(const std::vector<float>& mixed_interleaved, int frame_count, int64_t pts_100ns);
  void DrainAudioPacketsLocked(int64_t fallback_pts_100ns);
  void NormalizeAudioPacketTimestamps(AVPacket* packet, int64_t fallback_pts_100ns);
  void FlushEncoderLocked();
  void CleanupEncoderLocked();
  void SetError(std::string message);
  void ClearError();

  static constexpr int kOutputSampleRate = 44100;
  static constexpr int kOutputChannels = 2;

  PacketRingBuffer* ring_buffer_ = nullptr;
  int64_t qpc_origin_ = 0;
  int64_t qpc_frequency_ = 0;

  std::atomic<bool> running_{false};
  mutable std::mutex state_mutex_;
  std::vector<MicrophoneInfo> microphones_;
  int selected_microphone_index_ = 0;
  std::string last_error_;

  winrt::com_ptr<IMMDeviceEnumerator> device_enumerator_;
  CaptureContext loopback_context_;
  CaptureContext microphone_context_;
  SourceBuffer game_audio_buffer_;
  SourceBuffer microphone_buffer_;
  std::thread mixer_thread_;

  mutable std::mutex encoder_mutex_;
  AVCodecContext* audio_codec_ctx_ = nullptr;
  int64_t next_audio_pts_100ns_ = AV_NOPTS_VALUE;
  int64_t last_packet_pts_ = AV_NOPTS_VALUE;
  int64_t last_packet_dts_ = AV_NOPTS_VALUE;
  std::condition_variable mixer_cv_;
  std::mutex mixer_cv_mutex_;
};

class CaptureEngine {
 public:
  CaptureEngine() = default;
  ~CaptureEngine();

  bool Initialize(ID3D11Device* device,
                  ID3D11DeviceContext* context,
                  HWND ui_hwnd,
                  std::filesystem::path clips_directory);
  void Shutdown();

  void SetTargetMode(TargetMode mode);
  TargetMode target_mode() const;
  EngineStatus status() const;
  bool is_ready() const;
  bool is_saving() const;
  std::string last_error() const;
  std::string selected_codec() const;
  double buffered_seconds() const;
  std::size_t buffered_packets() const;
  std::filesystem::path clips_directory() const;
  PacketRingBuffer* packet_buffer() { return &packet_buffer_; }
  int64_t qpc_origin() const { return qpc_origin_; }
  int64_t qpc_frequency() const { return qpc_frequency_; }
  void SetAudioSnapshotProvider(std::function<bool(CodecSnapshot&)> provider);
  bool RequestClip();

 private:
  struct RawFrame {
    winrt::com_ptr<ID3D11Texture2D> bgra_texture;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t pts_100ns = 0;
  };

  struct ConvertedFrame {
    winrt::com_ptr<ID3D11Texture2D> nv12_texture;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t pts_100ns = 0;
    uint64_t fence_value = 0;
  };

  enum class CaptureKind {
    kWindow,
    kMonitor,
  };

  struct CaptureTarget {
    CaptureKind kind = CaptureKind::kMonitor;
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;

    bool operator==(const CaptureTarget& other) const {
      return kind == other.kind && hwnd == other.hwnd && monitor == other.monitor;
    }

    bool operator!=(const CaptureTarget& other) const {
      return !(*this == other);
    }
  };

  bool EnableMultithreadProtection();
  bool QueryAdapterVendorId();
  bool CreateWgcDevice();
  bool CreateFence();
  bool CreateHwDeviceContext();
  void CleanupHwDevice();

  bool CreateHwFramesContextLocked(uint32_t width, uint32_t height);
  bool SupportsD3D11Frames(const AVCodec* codec) const;
  std::vector<std::string> BuildCodecPreference() const;
  static bool StartsWith(std::string_view value, std::string_view prefix);
  static void SetEncoderOptions(AVDictionary** options, const std::string& codec_name, uint32_t fps);
  bool OpenEncoderLocked(const std::string& codec_name, uint32_t width, uint32_t height);
  bool EnsureEncoderLocked(uint32_t width, uint32_t height);
  void FlushEncoderLocked();
  void ReleaseEncoderLocked();
  struct Nv12RecycleCookie {
    CaptureEngine* engine = nullptr;
    ID3D11Texture2D* texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  static void ReleaseTexture(void* opaque, uint8_t* data);
  bool EncodeTextureLocked(ID3D11Texture2D* nv12_texture, int64_t pts_100ns);
  void NormalizePacketTimestamps(AVPacket* packet, int64_t fallback_pts_100ns);
  void DrainPacketsLocked(int64_t fallback_pts_100ns);

  static bool IsWindowCloaked(HWND hwnd);
  bool IsWindowCandidate(HWND hwnd) const;
  CaptureTarget DeterminePreferredTarget();
  static winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateItemForWindow(HWND hwnd);
  static winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateItemForMonitor(HMONITOR monitor);
  bool StartCaptureForTargetLocked(const CaptureTarget& target);
  void StopCaptureSessionLocked();
  void CleanupCaptureSession();

  bool EnsureVideoProcessor(uint32_t width, uint32_t height);
  bool RecreateNv12Pool(uint32_t width, uint32_t height);
  winrt::com_ptr<ID3D11Texture2D> CreateNv12Texture(uint32_t width, uint32_t height) const;
  winrt::com_ptr<ID3D11Texture2D> AcquireNv12Texture(uint32_t width, uint32_t height);
  void RecycleNv12Texture(ID3D11Texture2D* texture, uint32_t width, uint32_t height);
  winrt::com_ptr<ID3D11Texture2D> AcquireTextureFromSurface(
      const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) const;
  int64_t QueryTimestamp100ns() const;
  bool ConvertFrameToNv12(const RawFrame& raw, ConvertedFrame& converted);
  void OnFrameArrived(
      winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
      winrt::Windows::Foundation::IInspectable const&);
  void ProcessingLoop();
  void EncodeLoop();
  void TargetLoop();

  bool CopyCodecSnapshot(CodecSnapshot& snapshot);
  bool CopyAudioCodecSnapshot(CodecSnapshot& snapshot) const;
  std::wstring BuildOutputPath() const;
  static std::string WideToUtf8(const std::wstring& value);
  static bool WriteMp4File(const std::wstring& path,
                           const std::vector<BufferedPacket>& packets,
                           const CodecSnapshot& video_snapshot,
                           const CodecSnapshot* audio_snapshot);
  void SaveClipWorker(std::wstring output_path, double seconds);

  void CleanupFenceObjects();
  void CleanupEncoder();
  static void JoinThread(std::thread& thread);
  void AppendLog(std::string_view message) const;
  void SetError(std::string message);
  void ClearError();

  static constexpr uint32_t kFramePoolSize = 4;
  HWND ui_hwnd_ = nullptr;
  std::filesystem::path clips_directory_;
  std::filesystem::path log_path_;

  std::atomic<bool> initialized_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> saving_{false};
  std::atomic<bool> buffered_{false};
  std::atomic<bool> target_dirty_{false};
  std::atomic<TargetMode> target_mode_{TargetMode::kActiveWindow};
  std::atomic<uint64_t> fence_counter_{0};
  mutable std::mutex state_mutex_;
  mutable std::mutex log_mutex_;
  std::string last_error_;
  std::string selected_codec_name_;

  winrt::com_ptr<ID3D11Device> device_;
  winrt::com_ptr<ID3D11DeviceContext> context_;
  winrt::com_ptr<ID3D11Device5> device5_;
  winrt::com_ptr<ID3D11DeviceContext4> context4_;
  winrt::com_ptr<ID3D11VideoDevice> video_device_;
  winrt::com_ptr<ID3D11VideoContext> video_context_;
  winrt::com_ptr<ID3D11Fence> fence_;
  HANDLE fence_event_ = nullptr;

  winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice wgc_device_{nullptr};

  std::mutex session_mutex_;
  CaptureTarget active_target_{};
  HWND last_window_target_ = nullptr;
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
  mutable std::mutex nv12_pool_mutex_;
  std::vector<winrt::com_ptr<ID3D11Texture2D>> nv12_pool_;
  uint32_t nv12_pool_width_ = 0;
  uint32_t nv12_pool_height_ = 0;

  uint32_t adapter_vendor_id_ = 0;
  uint32_t fps_ = 60;
  int64_t qpc_frequency_ = 0;
  int64_t qpc_origin_ = 0;

  std::mutex encoder_mutex_;
  AVBufferRef* hw_device_ctx_ = nullptr;
  AVBufferRef* hw_frames_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  uint32_t encoder_width_ = 0;
  uint32_t encoder_height_ = 0;
  int64_t last_packet_pts_ = AV_NOPTS_VALUE;
  int64_t last_packet_dts_ = AV_NOPTS_VALUE;

  PacketRingBuffer packet_buffer_{16384, AVRational{1, 10'000'000}};
  SpscRingQueue<RawFrame, 256> raw_queue_;
  SpscRingQueue<ConvertedFrame, 128> encode_queue_;
  std::function<bool(CodecSnapshot&)> audio_snapshot_provider_;

  std::thread processing_thread_;
  std::thread encode_thread_;
  std::thread target_thread_;
  std::thread save_thread_;
};

struct AppContext {
  D3DState* d3d = nullptr;
  CaptureEngine* engine = nullptr;
  HWND hwnd = nullptr;
  bool save_hotkey_registered = false;
  bool toggle_hotkey_registered = false;
  bool ui_visible = true;
};

std::string StatusLabel(EngineStatus status);
ImVec4 StatusColor(EngineStatus status);
bool CreateRenderTarget(D3DState& d3d);
void CleanupRenderTarget(D3DState& d3d);
bool CreateDeviceD3D(HWND hwnd, D3DState& d3d);
void CleanupDeviceD3D(D3DState& d3d);
void ApplyUiStyle();
void RenderMainWindow(CaptureEngine& engine, WASAPIAudioCapture& audio, bool hotkey_registered);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

CodecSnapshot::CodecSnapshot(CodecSnapshot&& other) noexcept
    : params(std::exchange(other.params, nullptr)),
      time_base(other.time_base) {}

CodecSnapshot& CodecSnapshot::operator=(CodecSnapshot&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (params != nullptr) {
    avcodec_parameters_free(&params);
  }

  params = std::exchange(other.params, nullptr);
  time_base = other.time_base;
  return *this;
}

CodecSnapshot::~CodecSnapshot() {
  if (params != nullptr) {
    avcodec_parameters_free(&params);
  }
}

PacketRingBuffer::PacketRingBuffer(std::size_t capacity, AVRational time_base)
    : packets_(std::max<std::size_t>(capacity, 2)),
      time_base_(time_base) {}

void PacketRingBuffer::Clear() {
  std::scoped_lock lock(mutex_);
  for (auto& packet : packets_) {
    packet.packet.reset();
  }
  head_ = 0;
  count_ = 0;
}

bool PacketRingBuffer::Push(const AVPacket* packet, PacketKind kind) {
  if (packet == nullptr) {
    return false;
  }

  AvPacketPtr clone(av_packet_clone(packet));
  if (!clone) {
    return false;
  }

  std::scoped_lock lock(mutex_);
  packets_[head_].packet = std::move(clone);
  packets_[head_].kind = kind;
  head_ = (head_ + 1) % packets_.size();
  count_ = std::min(count_ + 1, packets_.size());
  return true;
}

bool PacketRingBuffer::empty() const {
  std::scoped_lock lock(mutex_);
  return count_ == 0;
}

std::size_t PacketRingBuffer::size() const {
  std::scoped_lock lock(mutex_);
  return count_;
}

std::vector<BufferedPacket> PacketRingBuffer::CloneOrderedPackets() const {
  std::vector<BufferedPacket> ordered;

  std::scoped_lock lock(mutex_);
  ordered.reserve(count_);
  const std::size_t start = (head_ + packets_.size() - count_) % packets_.size();
  for (std::size_t i = 0; i < count_; ++i) {
    const std::size_t index = (start + i) % packets_.size();
    if (packets_[index].packet) {
      BufferedPacket clone{};
      clone.packet.reset(av_packet_clone(packets_[index].packet.get()));
      clone.kind = packets_[index].kind;
      ordered.push_back(std::move(clone));
    }
  }

  return ordered;
}

double PacketRingBuffer::BufferedSeconds() const {
  std::vector<BufferedPacket> ordered = CloneOrderedPackets();
  if (ordered.size() < 2) {
    return 0.0;
  }

  int64_t first_pts = AV_NOPTS_VALUE;
  int64_t last_pts = AV_NOPTS_VALUE;
  for (const auto& packet : ordered) {
    if (packet.kind != PacketKind::kVideo || !packet.packet || packet.packet->pts == AV_NOPTS_VALUE) {
      continue;
    }
    if (first_pts == AV_NOPTS_VALUE) {
      first_pts = packet.packet->pts;
    }
    last_pts = packet.packet->pts;
  }

  if (first_pts == AV_NOPTS_VALUE || last_pts == AV_NOPTS_VALUE || last_pts < first_pts) {
    return 0.0;
  }

  return static_cast<double>(last_pts - first_pts) / 10'000'000.0;
}

std::vector<BufferedPacket> PacketRingBuffer::SnapshotLastWindow(double seconds) const {
  std::vector<BufferedPacket> ordered = CloneOrderedPackets();
  if (ordered.empty()) {
    return {};
  }

  int64_t latest_pts = AV_NOPTS_VALUE;
  for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
    if (it->kind == PacketKind::kVideo && it->packet && it->packet->pts != AV_NOPTS_VALUE) {
      latest_pts = it->packet->pts;
      break;
    }
  }

  if (latest_pts == AV_NOPTS_VALUE) {
    return ordered;
  }

  const int64_t threshold_pts = latest_pts - static_cast<int64_t>(seconds * 10'000'000.0);
  std::size_t candidate_index = 0;
  bool found_candidate = false;
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    if (ordered[i].kind != PacketKind::kVideo || !ordered[i].packet || ordered[i].packet->pts == AV_NOPTS_VALUE) {
      continue;
    }
    if (ordered[i].packet->pts >= threshold_pts) {
      candidate_index = i;
      found_candidate = true;
      break;
    }
  }

  if (!found_candidate) {
    candidate_index = 0;
  }

  std::size_t start_index = candidate_index;
  for (std::size_t i = candidate_index + 1; i > 0; --i) {
    const std::size_t idx = i - 1;
    if (ordered[idx].kind == PacketKind::kVideo && ordered[idx].packet &&
        (ordered[idx].packet->flags & AV_PKT_FLAG_KEY) != 0) {
      start_index = idx;
      break;
    }
  }

  if (ordered[start_index].kind != PacketKind::kVideo || ordered[start_index].packet == nullptr ||
      (ordered[start_index].packet->flags & AV_PKT_FLAG_KEY) == 0) {
    for (std::size_t i = candidate_index; i < ordered.size(); ++i) {
      if (ordered[i].kind == PacketKind::kVideo && ordered[i].packet &&
          (ordered[i].packet->flags & AV_PKT_FLAG_KEY) != 0) {
        start_index = i;
        break;
      }
    }
  }

  const int64_t clip_start_pts = (ordered[start_index].packet != nullptr)
                                     ? ordered[start_index].packet->pts
                                     : threshold_pts;

  std::vector<BufferedPacket> result;
  result.reserve(ordered.size() - start_index);
  for (std::size_t i = start_index; i < ordered.size(); ++i) {
    if (ordered[i].packet && ordered[i].packet->pts != AV_NOPTS_VALUE &&
        ordered[i].packet->pts >= clip_start_pts) {
      result.push_back(std::move(ordered[i]));
    }
  }

  std::sort(result.begin(), result.end(), [](const BufferedPacket& lhs, const BufferedPacket& rhs) {
    const int64_t lhs_dts = (lhs.packet && lhs.packet->dts != AV_NOPTS_VALUE) ? lhs.packet->dts : lhs.packet->pts;
    const int64_t rhs_dts = (rhs.packet && rhs.packet->dts != AV_NOPTS_VALUE) ? rhs.packet->dts : rhs.packet->pts;
    if (lhs_dts == rhs_dts) {
      return lhs.kind == PacketKind::kVideo && rhs.kind == PacketKind::kAudio;
    }
    return lhs_dts < rhs_dts;
  });

  return result;
}

std::string WASAPIAudioCapture::WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }

  std::string utf8(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), required, nullptr, nullptr);
  if (!utf8.empty() && utf8.back() == '\0') {
    utf8.pop_back();
  }
  return utf8;
}

int64_t WASAPIAudioCapture::SamplesTo100ns(int sample_count, int sample_rate) {
  return (static_cast<int64_t>(sample_count) * 10'000'000LL) / sample_rate;
}

int64_t WASAPIAudioCapture::FramesTo100ns(int frame_count, int sample_rate) {
  return SamplesTo100ns(frame_count, sample_rate);
}

int WASAPIAudioCapture::PtsToFrames(int64_t pts_100ns, int sample_rate) {
  return static_cast<int>((pts_100ns * sample_rate) / 10'000'000LL);
}

AVSampleFormat WASAPIAudioCapture::WaveFormatToAvSampleFormat(const WAVEFORMATEX* mix_format) {
  if (mix_format == nullptr) {
    return AV_SAMPLE_FMT_NONE;
  }

  WORD format_tag = mix_format->wFormatTag;
  WORD bits = mix_format->wBitsPerSample;

  if (format_tag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_format);
    if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
      format_tag = WAVE_FORMAT_IEEE_FLOAT;
    } else if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
      format_tag = WAVE_FORMAT_PCM;
    }
    bits = extensible->Samples.wValidBitsPerSample != 0
               ? extensible->Samples.wValidBitsPerSample
               : mix_format->wBitsPerSample;
  }

  if (format_tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) {
    return AV_SAMPLE_FMT_FLT;
  }
  if (format_tag == WAVE_FORMAT_PCM && bits == 16) {
    return AV_SAMPLE_FMT_S16;
  }
  if (format_tag == WAVE_FORMAT_PCM && bits == 32) {
    return AV_SAMPLE_FMT_S32;
  }

  return AV_SAMPLE_FMT_NONE;
}

bool WASAPIAudioCapture::RefreshMicrophones() {
  microphones_.clear();

  winrt::com_ptr<IMMDeviceCollection> collection;
  if (FAILED(device_enumerator_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.put()))) {
    return false;
  }

  UINT count = 0;
  if (FAILED(collection->GetCount(&count))) {
    return false;
  }

  for (UINT i = 0; i < count; ++i) {
    winrt::com_ptr<IMMDevice> device;
    if (FAILED(collection->Item(i, device.put()))) {
      continue;
    }

    LPWSTR device_id = nullptr;
    if (FAILED(device->GetId(&device_id))) {
      continue;
    }

    winrt::com_ptr<IPropertyStore> property_store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, property_store.put()))) {
      CoTaskMemFree(device_id);
      continue;
    }

    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring friendly_name = L"Microphone";
    if (SUCCEEDED(property_store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
      friendly_name = value.pwszVal;
    }
    PropVariantClear(&value);

    microphones_.push_back(MicrophoneInfo{std::wstring(device_id), WideToUtf8(friendly_name)});
    CoTaskMemFree(device_id);
  }

  if (selected_microphone_index_ >= static_cast<int>(microphones_.size())) {
    selected_microphone_index_ = microphones_.empty() ? -1 : 0;
  }

  return true;
}

bool WASAPIAudioCapture::InitializeAudioEncoder() {
  const AVCodec* codec = avcodec_find_encoder_by_name("aac");
  if (codec == nullptr) {
    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  }
  if (codec == nullptr) {
    SetError("Failed to find the AAC encoder.");
    return false;
  }

  audio_codec_ctx_ = avcodec_alloc_context3(codec);
  if (audio_codec_ctx_ == nullptr) {
    SetError("Failed to allocate the AAC codec context.");
    return false;
  }

  audio_codec_ctx_->sample_rate = kOutputSampleRate;
  audio_codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  audio_codec_ctx_->time_base = AVRational{1, 10'000'000};
  audio_codec_ctx_->pkt_timebase = audio_codec_ctx_->time_base;
  audio_codec_ctx_->bit_rate = 192'000;
  av_channel_layout_default(&audio_codec_ctx_->ch_layout, kOutputChannels);

  if (avcodec_open2(audio_codec_ctx_, codec, nullptr) < 0) {
    SetError("Failed to open the AAC encoder.");
    avcodec_free_context(&audio_codec_ctx_);
    return false;
  }

  return true;
}

bool WASAPIAudioCapture::OpenCaptureContext(CaptureContext& context,
                                            const std::wstring& device_id,
                                            DWORD stream_flags) {
  context.device_id = device_id;

  if (device_id.empty()) {
    if (FAILED(device_enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, context.device.put()))) {
      return false;
    }
  } else {
    if (FAILED(device_enumerator_->GetDevice(device_id.c_str(), context.device.put()))) {
      return false;
    }
  }

  if (FAILED(context.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, context.audio_client.put_void()))) {
    return false;
  }

  if (FAILED(context.audio_client->GetMixFormat(&context.mix_format))) {
    return false;
  }

  const AVSampleFormat input_fmt = WaveFormatToAvSampleFormat(context.mix_format);
  if (input_fmt == AV_SAMPLE_FMT_NONE) {
    return false;
  }

  context.sample_ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (context.sample_ready_event == nullptr) {
    return false;
  }

  const HRESULT init_result = context.audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      stream_flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
      0,
      0,
      context.mix_format,
      nullptr);
  if (FAILED(init_result)) {
    return false;
  }

  if (FAILED(context.audio_client->SetEventHandle(context.sample_ready_event))) {
    return false;
  }

  if (FAILED(context.audio_client->GetService(__uuidof(IAudioCaptureClient), context.capture_client.put_void()))) {
    return false;
  }

  AVChannelLayout in_layout{};
  AVChannelLayout out_layout{};
  av_channel_layout_default(&in_layout, context.mix_format->nChannels);
  av_channel_layout_default(&out_layout, kOutputChannels);

  if (swr_alloc_set_opts2(&context.swr,
                          &out_layout,
                          AV_SAMPLE_FMT_FLT,
                          kOutputSampleRate,
                          &in_layout,
                          input_fmt,
                          context.mix_format->nSamplesPerSec,
                          0,
                          nullptr) < 0) {
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    return false;
  }

  av_channel_layout_uninit(&in_layout);
  av_channel_layout_uninit(&out_layout);

  if (swr_init(context.swr) < 0) {
    swr_free(&context.swr);
    return false;
  }

  return true;
}

void WASAPIAudioCapture::StopCaptureContext(CaptureContext& context) {
  context.running.store(false, std::memory_order_release);
  if (context.sample_ready_event != nullptr) {
    SetEvent(context.sample_ready_event);
  }
  if (context.audio_client != nullptr) {
    context.audio_client->Stop();
  }
  if (context.thread.joinable()) {
    context.thread.join();
  }
  if (context.swr != nullptr) {
    swr_free(&context.swr);
  }
  if (context.mix_format != nullptr) {
    CoTaskMemFree(context.mix_format);
    context.mix_format = nullptr;
  }
  if (context.sample_ready_event != nullptr) {
    CloseHandle(context.sample_ready_event);
    context.sample_ready_event = nullptr;
  }
  context.convert_buffer.clear();
  context.capture_client = nullptr;
  context.audio_client = nullptr;
  context.device = nullptr;
}

bool WASAPIAudioCapture::StartLoopbackCapture() {
  loopback_context_.kind = SourceKind::kLoopback;
  if (!OpenCaptureContext(loopback_context_, L"", AUDCLNT_STREAMFLAGS_LOOPBACK)) {
    SetError("Failed to start WASAPI loopback capture.");
    return false;
  }

  loopback_context_.running.store(true, std::memory_order_release);
  loopback_context_.thread =
      std::thread(&WASAPIAudioCapture::CaptureThreadMain, this, &loopback_context_, &game_audio_buffer_);
  return true;
}

bool WASAPIAudioCapture::StartMicrophoneCaptureLocked(int index) {
  if (index < 0 || index >= static_cast<int>(microphones_.size())) {
    return false;
  }

  microphone_context_.kind = SourceKind::kMicrophone;
  if (!OpenCaptureContext(microphone_context_, microphones_[index].id, 0)) {
    SetError("Failed to start the selected microphone capture.");
    return false;
  }

  microphone_context_.running.store(true, std::memory_order_release);
  microphone_context_.thread =
      std::thread(&WASAPIAudioCapture::CaptureThreadMain, this, &microphone_context_, &microphone_buffer_);
  return true;
}

bool WASAPIAudioCapture::Initialize(PacketRingBuffer* ring_buffer, int64_t qpc_origin, int64_t qpc_frequency) {
  Shutdown();

  ring_buffer_ = ring_buffer;
  qpc_origin_ = qpc_origin;
  qpc_frequency_ = qpc_frequency;

  if (ring_buffer_ == nullptr) {
    SetError("Audio capture was not given a shared packet ring.");
    return false;
  }

  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                              nullptr,
                              CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              device_enumerator_.put_void()))) {
    SetError("Failed to create IMMDeviceEnumerator.");
    return false;
  }

  RefreshMicrophones();
  if (!InitializeAudioEncoder()) {
    return false;
  }

  running_.store(true, std::memory_order_release);

  if (!StartLoopbackCapture()) {
    running_.store(false, std::memory_order_release);
    return false;
  }

  if (!microphones_.empty()) {
    StartMicrophoneCaptureLocked(selected_microphone_index_);
  }

  mixer_thread_ = std::thread(&WASAPIAudioCapture::MixerThreadMain, this);
  ClearError();
  return true;
}

void WASAPIAudioCapture::Shutdown() {
  running_.store(false, std::memory_order_release);
  mixer_cv_.notify_all();
  StopCaptureContext(loopback_context_);
  StopCaptureContext(microphone_context_);
  if (mixer_thread_.joinable()) {
    mixer_thread_.join();
  }

  std::scoped_lock lock(encoder_mutex_);
  FlushEncoderLocked();
  CleanupEncoderLocked();

  {
    std::scoped_lock game_lock(game_audio_buffer_.mutex);
    game_audio_buffer_.samples.clear();
    game_audio_buffer_.read_pts_100ns = AV_NOPTS_VALUE;
  }
  {
    std::scoped_lock mic_lock(microphone_buffer_.mutex);
    microphone_buffer_.samples.clear();
    microphone_buffer_.read_pts_100ns = AV_NOPTS_VALUE;
  }

  device_enumerator_ = nullptr;
}

std::vector<WASAPIAudioCapture::MicrophoneInfo> WASAPIAudioCapture::microphones() const {
  std::scoped_lock lock(state_mutex_);
  return microphones_;
}

int WASAPIAudioCapture::selected_microphone_index() const {
  std::scoped_lock lock(state_mutex_);
  return selected_microphone_index_;
}

void WASAPIAudioCapture::SetSelectedMicrophoneIndex(int index) {
  std::scoped_lock lock(state_mutex_);
  if (index < 0 || index >= static_cast<int>(microphones_.size()) || index == selected_microphone_index_) {
    return;
  }

  selected_microphone_index_ = index;
  {
    std::scoped_lock mic_lock(microphone_buffer_.mutex);
    microphone_buffer_.samples.clear();
    microphone_buffer_.read_pts_100ns = AV_NOPTS_VALUE;
  }

  StopCaptureContext(microphone_context_);
  StartMicrophoneCaptureLocked(selected_microphone_index_);
  mixer_cv_.notify_all();
}

bool WASAPIAudioCapture::game_audio_active() const {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  const int64_t now_100ns = ((counter.QuadPart - qpc_origin_) * 10'000'000LL) / qpc_frequency_;
  return (now_100ns - game_audio_buffer_.last_activity_100ns.load(std::memory_order_acquire)) < 5'000'000;
}

bool WASAPIAudioCapture::microphone_active() const {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  const int64_t now_100ns = ((counter.QuadPart - qpc_origin_) * 10'000'000LL) / qpc_frequency_;
  return (now_100ns - microphone_buffer_.last_activity_100ns.load(std::memory_order_acquire)) < 5'000'000;
}

float WASAPIAudioCapture::QueryLevel(const SourceBuffer& buffer) const {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  const int64_t now_100ns = ((counter.QuadPart - qpc_origin_) * 10'000'000LL) / qpc_frequency_;
  const int64_t age = now_100ns - buffer.peak_pts_100ns.load(std::memory_order_acquire);
  if (age >= 3'000'000) {
    return 0.0f;
  }

  const float raw_level = buffer.peak_level.load(std::memory_order_acquire);
  const float decay = std::clamp(1.0f - (static_cast<float>(age) / 3'000'000.0f), 0.0f, 1.0f);
  return raw_level * decay;
}

float WASAPIAudioCapture::game_audio_level() const {
  return QueryLevel(game_audio_buffer_);
}

float WASAPIAudioCapture::microphone_level() const {
  return QueryLevel(microphone_buffer_);
}

bool WASAPIAudioCapture::CopyCodecSnapshot(CodecSnapshot& snapshot) const {
  std::scoped_lock lock(encoder_mutex_);
  if (audio_codec_ctx_ == nullptr) {
    return false;
  }

  snapshot.params = avcodec_parameters_alloc();
  if (snapshot.params == nullptr) {
    return false;
  }

  if (avcodec_parameters_from_context(snapshot.params, audio_codec_ctx_) < 0) {
    avcodec_parameters_free(&snapshot.params);
    return false;
  }

  snapshot.time_base = audio_codec_ctx_->time_base;
  return true;
}

void WASAPIAudioCapture::AppendSourceSamples(SourceBuffer& buffer,
                                             int64_t pts_100ns,
                                             const float* samples,
                                             int frame_count) {
  if (frame_count <= 0 || samples == nullptr) {
    return;
  }

  int sample_offset = 0;
  int sample_count = frame_count;
  std::scoped_lock lock(buffer.mutex);
  if (buffer.read_pts_100ns == AV_NOPTS_VALUE) {
    buffer.read_pts_100ns = pts_100ns;
  } else {
    const int queued_frames = static_cast<int>(buffer.samples.size() / kOutputChannels);
    const int64_t expected_pts = buffer.read_pts_100ns + FramesTo100ns(queued_frames, kOutputSampleRate);
    const int delta_frames = PtsToFrames(pts_100ns - expected_pts, kOutputSampleRate);
    if (delta_frames > 0) {
      buffer.samples.insert(buffer.samples.end(), delta_frames * kOutputChannels, 0.0f);
    } else if (delta_frames < 0) {
      const int overlap_frames = std::min(-delta_frames, frame_count);
      sample_offset = overlap_frames * kOutputChannels;
      sample_count -= overlap_frames;
    }
  }

  if (sample_count > 0) {
    float peak = 0.0f;
    for (int i = sample_offset; i < sample_offset + (sample_count * kOutputChannels); ++i) {
      peak = std::max(peak, std::abs(samples[i]));
    }
    buffer.samples.insert(buffer.samples.end(),
                          samples + sample_offset,
                          samples + sample_offset + (sample_count * kOutputChannels));
    buffer.last_activity_100ns.store(pts_100ns + FramesTo100ns(frame_count, kOutputSampleRate),
                                     std::memory_order_release);
    buffer.peak_level.store(peak, std::memory_order_release);
    buffer.peak_pts_100ns.store(pts_100ns + FramesTo100ns(frame_count, kOutputSampleRate),
                                std::memory_order_release);
  }
  mixer_cv_.notify_one();
}

void WASAPIAudioCapture::CaptureThreadMain(CaptureContext* context, SourceBuffer* buffer) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  if (context->audio_client == nullptr || context->capture_client == nullptr || context->mix_format == nullptr) {
    CoUninitialize();
    return;
  }

  context->audio_client->Start();

  while (running_.load(std::memory_order_acquire) && context->running.load(std::memory_order_acquire)) {
    const DWORD wait_result =
        WaitForSingleObject(context->sample_ready_event, 100);
    if (wait_result != WAIT_OBJECT_0) {
      continue;
    }

    UINT32 packet_size = 0;
    if (FAILED(context->capture_client->GetNextPacketSize(&packet_size)) || packet_size == 0) {
      continue;
    }

    while (packet_size > 0) {
      BYTE* data = nullptr;
      UINT32 frame_count = 0;
      DWORD flags = 0;
      UINT64 device_position = 0;
      UINT64 qpc_position = 0;
      if (FAILED(context->capture_client->GetBuffer(
              &data, &frame_count, &flags, &device_position, &qpc_position))) {
        break;
      }

      const AVSampleFormat input_fmt = WaveFormatToAvSampleFormat(context->mix_format);
      const int out_capacity = av_rescale_rnd(
          swr_get_delay(context->swr, context->mix_format->nSamplesPerSec) + frame_count,
          kOutputSampleRate,
          context->mix_format->nSamplesPerSec,
          AV_ROUND_UP);

      context->convert_buffer.resize(static_cast<std::size_t>(out_capacity) * kOutputChannels);
      uint8_t* output_planes[1] = {reinterpret_cast<uint8_t*>(context->convert_buffer.data())};
      const uint8_t* input_planes[1] = {
          (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data,
      };

      const int converted = swr_convert(context->swr,
                                        output_planes,
                                        out_capacity,
                                        input_planes,
                                        static_cast<int>(frame_count));
      if (converted > 0) {
        const int64_t pts_100ns = qpc_position > static_cast<UINT64>(qpc_origin_)
                                      ? static_cast<int64_t>((qpc_position - qpc_origin_) * 10'000'000ULL /
                                                             static_cast<UINT64>(qpc_frequency_))
                                      : 0;
        AppendSourceSamples(*buffer, pts_100ns, context->convert_buffer.data(), converted);
      }

      context->capture_client->ReleaseBuffer(frame_count);
      if (FAILED(context->capture_client->GetNextPacketSize(&packet_size))) {
        packet_size = 0;
      }
    }
  }

  context->audio_client->Stop();
  CoUninitialize();
}

bool WASAPIAudioCapture::MixNextFrame(std::vector<float>& mixed_interleaved,
                                      int& mixed_frame_count,
                                      int64_t& pts_100ns) {
  mixed_frame_count = (audio_codec_ctx_ != nullptr && audio_codec_ctx_->frame_size > 0)
                          ? audio_codec_ctx_->frame_size
                          : 1024;

  if (next_audio_pts_100ns_ == AV_NOPTS_VALUE) {
    int64_t game_pts = AV_NOPTS_VALUE;
    int64_t mic_pts = AV_NOPTS_VALUE;
    {
      std::scoped_lock game_lock(game_audio_buffer_.mutex);
      game_pts = game_audio_buffer_.read_pts_100ns;
    }
    {
      std::scoped_lock mic_lock(microphone_buffer_.mutex);
      mic_pts = microphone_buffer_.read_pts_100ns;
    }
    if (game_pts == AV_NOPTS_VALUE && mic_pts == AV_NOPTS_VALUE) {
      return false;
    }

    next_audio_pts_100ns_ = (game_pts == AV_NOPTS_VALUE)
                                ? mic_pts
                                : (mic_pts == AV_NOPTS_VALUE ? game_pts : std::min(game_pts, mic_pts));
  }

  pts_100ns = next_audio_pts_100ns_;
  mixed_interleaved.assign(static_cast<std::size_t>(mixed_frame_count) * kOutputChannels, 0.0f);

  auto mix_source = [&](SourceBuffer& source) {
    std::scoped_lock lock(source.mutex);
    if (source.read_pts_100ns == AV_NOPTS_VALUE || source.samples.empty()) {
      return false;
    }

    if (source.read_pts_100ns < pts_100ns) {
      const int drop_frames = std::min(PtsToFrames(pts_100ns - source.read_pts_100ns, kOutputSampleRate),
                                       static_cast<int>(source.samples.size() / kOutputChannels));
      if (drop_frames > 0) {
        source.samples.erase(source.samples.begin(), source.samples.begin() + drop_frames * kOutputChannels);
        source.read_pts_100ns += FramesTo100ns(drop_frames, kOutputSampleRate);
      }
    }

    if (source.read_pts_100ns >= pts_100ns + FramesTo100ns(mixed_frame_count, kOutputSampleRate)) {
      return false;
    }

    const int offset_frames =
        std::max(0, PtsToFrames(source.read_pts_100ns - pts_100ns, kOutputSampleRate));
    const int available_frames = static_cast<int>(source.samples.size() / kOutputChannels);
    const int take_frames = std::min(mixed_frame_count - offset_frames, available_frames);
    if (take_frames <= 0) {
      return false;
    }

    for (int frame = 0; frame < take_frames; ++frame) {
      const int src = frame * kOutputChannels;
      const int dst = (offset_frames + frame) * kOutputChannels;
      mixed_interleaved[dst + 0] += source.samples[src + 0];
      mixed_interleaved[dst + 1] += source.samples[src + 1];
    }

    source.samples.erase(source.samples.begin(), source.samples.begin() + take_frames * kOutputChannels);
    source.read_pts_100ns += FramesTo100ns(take_frames, kOutputSampleRate);
    return true;
  };

  bool has_audio = false;
  has_audio |= mix_source(game_audio_buffer_);
  has_audio |= mix_source(microphone_buffer_);
  if (!has_audio) {
    return false;
  }

  for (float& sample : mixed_interleaved) {
    sample = std::clamp(sample, -1.0f, 1.0f);
  }

  next_audio_pts_100ns_ += FramesTo100ns(mixed_frame_count, kOutputSampleRate);
  return true;
}

void WASAPIAudioCapture::NormalizeAudioPacketTimestamps(AVPacket* packet, int64_t fallback_pts_100ns) {
  const int64_t nominal_duration = FramesTo100ns(
      (audio_codec_ctx_ != nullptr && audio_codec_ctx_->frame_size > 0) ? audio_codec_ctx_->frame_size : 1024,
      kOutputSampleRate);

  int64_t pts = packet->pts == AV_NOPTS_VALUE ? fallback_pts_100ns : packet->pts;
  if (last_packet_pts_ != AV_NOPTS_VALUE && pts <= last_packet_pts_) {
    pts = last_packet_pts_ + nominal_duration;
  }

  int64_t dts = packet->dts == AV_NOPTS_VALUE ? pts : packet->dts;
  if (last_packet_dts_ != AV_NOPTS_VALUE && dts <= last_packet_dts_) {
    dts = last_packet_dts_ + nominal_duration;
  }
  if (dts > pts) {
    pts = dts;
  }

  packet->pts = pts;
  packet->dts = dts;
  if (packet->duration <= 0) {
    packet->duration = nominal_duration;
  }

  last_packet_pts_ = packet->pts;
  last_packet_dts_ = packet->dts;
}

void WASAPIAudioCapture::DrainAudioPacketsLocked(int64_t fallback_pts_100ns) {
  if (audio_codec_ctx_ == nullptr || ring_buffer_ == nullptr) {
    return;
  }

  AvPacketPtr packet(av_packet_alloc());
  if (!packet) {
    return;
  }

  for (;;) {
    const int result = avcodec_receive_packet(audio_codec_ctx_, packet.get());
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
      break;
    }
    if (result < 0) {
      break;
    }

    NormalizeAudioPacketTimestamps(packet.get(), fallback_pts_100ns);
    ring_buffer_->Push(packet.get(), PacketKind::kAudio);
    av_packet_unref(packet.get());
  }
}

bool WASAPIAudioCapture::EncodeMixedFrame(const std::vector<float>& mixed_interleaved,
                                          int frame_count,
                                          int64_t pts_100ns) {
  if (audio_codec_ctx_ == nullptr || frame_count <= 0) {
    return false;
  }

  AVFrame* frame = av_frame_alloc();
  if (frame == nullptr) {
    return false;
  }

  frame->format = audio_codec_ctx_->sample_fmt;
  frame->nb_samples = frame_count;
  frame->sample_rate = audio_codec_ctx_->sample_rate;
  av_channel_layout_copy(&frame->ch_layout, &audio_codec_ctx_->ch_layout);
  frame->pts = pts_100ns;

  if (av_frame_get_buffer(frame, 0) < 0) {
    av_frame_free(&frame);
    return false;
  }

  auto* left = reinterpret_cast<float*>(frame->data[0]);
  auto* right = reinterpret_cast<float*>(frame->data[1]);
  for (int i = 0; i < frame_count; ++i) {
    left[i] = mixed_interleaved[static_cast<std::size_t>(i) * 2];
    right[i] = mixed_interleaved[static_cast<std::size_t>(i) * 2 + 1];
  }

  int send_result = avcodec_send_frame(audio_codec_ctx_, frame);
  av_frame_free(&frame);
  if (send_result < 0) {
    return false;
  }

  DrainAudioPacketsLocked(pts_100ns);
  return true;
}

void WASAPIAudioCapture::MixerThreadMain() {
  using namespace std::chrono_literals;

  std::vector<float> mixed_interleaved;
  while (running_.load(std::memory_order_acquire)) {
    int frame_count = 0;
    int64_t pts_100ns = AV_NOPTS_VALUE;
    if (!MixNextFrame(mixed_interleaved, frame_count, pts_100ns)) {
      std::unique_lock wait_lock(mixer_cv_mutex_);
      mixer_cv_.wait_for(wait_lock, 15ms);
      continue;
    }

    std::scoped_lock lock(encoder_mutex_);
    EncodeMixedFrame(mixed_interleaved, frame_count, pts_100ns);
  }
}

void WASAPIAudioCapture::FlushEncoderLocked() {
  if (audio_codec_ctx_ == nullptr) {
    return;
  }

  avcodec_send_frame(audio_codec_ctx_, nullptr);
  DrainAudioPacketsLocked(last_packet_pts_ == AV_NOPTS_VALUE ? 0 : last_packet_pts_);
}

void WASAPIAudioCapture::CleanupEncoderLocked() {
  if (audio_codec_ctx_ != nullptr) {
    avcodec_free_context(&audio_codec_ctx_);
    audio_codec_ctx_ = nullptr;
  }
  next_audio_pts_100ns_ = AV_NOPTS_VALUE;
  last_packet_pts_ = AV_NOPTS_VALUE;
  last_packet_dts_ = AV_NOPTS_VALUE;
}

void WASAPIAudioCapture::SetError(std::string message) {
  std::scoped_lock lock(state_mutex_);
  last_error_ = std::move(message);
}

void WASAPIAudioCapture::ClearError() {
  std::scoped_lock lock(state_mutex_);
  last_error_.clear();
}

CaptureEngine::~CaptureEngine() {
  Shutdown();
}

bool CaptureEngine::Initialize(ID3D11Device* device,
                               ID3D11DeviceContext* context,
                               HWND ui_hwnd,
                               std::filesystem::path clips_directory) {
  Shutdown();

  if (device == nullptr || context == nullptr) {
    SetError("Missing D3D11 device/context.");
    return false;
  }

  ui_hwnd_ = ui_hwnd;
  clips_directory_ = std::move(clips_directory);
  log_path_ = clips_directory_.parent_path() / "klip.log";
  std::error_code dir_ec{};
  std::filesystem::create_directories(clips_directory_, dir_ec);
  {
    std::ofstream log_stream(log_path_, std::ios::trunc);
    log_stream << "Klip session started\n";
  }

  device_.copy_from(device);
  context_.copy_from(context);

  if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
    SetError("Windows Graphics Capture is not supported on this system.");
    return false;
  }

  if (!EnableMultithreadProtection()) {
    SetError("Failed to enable D3D11 multithread protection.");
    return false;
  }

  if (FAILED(device_->QueryInterface(IID_PPV_ARGS(device5_.put()))) ||
      FAILED(context_->QueryInterface(IID_PPV_ARGS(context4_.put()))) ||
      FAILED(device_->QueryInterface(IID_PPV_ARGS(video_device_.put()))) ||
      FAILED(context_->QueryInterface(IID_PPV_ARGS(video_context_.put())))) {
    SetError("Failed to query required Direct3D 11.4 interfaces.");
    return false;
  }

  if (!CreateWgcDevice()) {
    SetError("Failed to create the WinRT D3D11 interop device.");
    return false;
  }

  if (!CreateFence()) {
    SetError("Failed to create the D3D11 fence used by the zero-copy pipeline.");
    return false;
  }

  fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) {
    SetError("Failed to create the fence wait event.");
    return false;
  }

  LARGE_INTEGER frequency{};
  LARGE_INTEGER counter{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  qpc_frequency_ = frequency.QuadPart;
  qpc_origin_ = counter.QuadPart;

  if (!QueryAdapterVendorId()) {
    SetError("Failed to query the D3D11 adapter information.");
    return false;
  }

  if (!CreateHwDeviceContext()) {
    SetError("Failed to initialize FFmpeg's D3D11 hardware device context.");
    return false;
  }

  target_mode_.store(TargetMode::kActiveWindow, std::memory_order_release);
  target_dirty_.store(true, std::memory_order_release);
  running_.store(true, std::memory_order_release);

  processing_thread_ = std::thread(&CaptureEngine::ProcessingLoop, this);
  encode_thread_ = std::thread(&CaptureEngine::EncodeLoop, this);
  target_thread_ = std::thread(&CaptureEngine::TargetLoop, this);

  initialized_.store(true, std::memory_order_release);
  ClearError();
  return true;
}

void CaptureEngine::Shutdown() {
  initialized_.store(false, std::memory_order_release);

  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    JoinThread(save_thread_);
    JoinThread(encode_thread_);
    JoinThread(processing_thread_);
    JoinThread(target_thread_);
    CleanupEncoder();
    CleanupFenceObjects();
    CleanupCaptureSession();
    {
      std::scoped_lock lock(nv12_pool_mutex_);
      nv12_pool_.clear();
      nv12_pool_width_ = 0;
      nv12_pool_height_ = 0;
    }
    device_ = nullptr;
    context_ = nullptr;
    return;
  }

  {
    std::scoped_lock lock(session_mutex_);
    StopCaptureSessionLocked();
  }

  JoinThread(target_thread_);
  JoinThread(processing_thread_);
  JoinThread(encode_thread_);

  CleanupEncoder();
  JoinThread(save_thread_);
  packet_buffer_.Clear();
  saving_.store(false, std::memory_order_release);
  buffered_.store(false, std::memory_order_release);

  CleanupFenceObjects();
  CleanupCaptureSession();
  CleanupHwDevice();
  {
    std::scoped_lock lock(nv12_pool_mutex_);
    nv12_pool_.clear();
    nv12_pool_width_ = 0;
    nv12_pool_height_ = 0;
  }

  device_ = nullptr;
  context_ = nullptr;
  device5_ = nullptr;
  context4_ = nullptr;
  video_device_ = nullptr;
  video_context_ = nullptr;
  wgc_device_ = nullptr;
  ui_hwnd_ = nullptr;
}

void CaptureEngine::SetTargetMode(TargetMode mode) {
  target_mode_.store(mode, std::memory_order_release);
  target_dirty_.store(true, std::memory_order_release);
}

TargetMode CaptureEngine::target_mode() const {
  return target_mode_.load(std::memory_order_acquire);
}

EngineStatus CaptureEngine::status() const {
  if (!initialized_.load(std::memory_order_acquire)) {
    return EngineStatus::kIdle;
  }

  if (saving_.load(std::memory_order_acquire)) {
    return EngineStatus::kSaving;
  }

  return EngineStatus::kBuffering;
}

bool CaptureEngine::is_ready() const {
  return initialized_.load(std::memory_order_acquire);
}

bool CaptureEngine::is_saving() const {
  return saving_.load(std::memory_order_acquire);
}

std::string CaptureEngine::last_error() const {
  std::scoped_lock lock(state_mutex_);
  return last_error_;
}

std::string CaptureEngine::selected_codec() const {
  std::scoped_lock lock(state_mutex_);
  return selected_codec_name_;
}

double CaptureEngine::buffered_seconds() const {
  return packet_buffer_.BufferedSeconds();
}

std::size_t CaptureEngine::buffered_packets() const {
  return packet_buffer_.size();
}

std::filesystem::path CaptureEngine::clips_directory() const {
  return clips_directory_;
}

void CaptureEngine::SetAudioSnapshotProvider(std::function<bool(CodecSnapshot&)> provider) {
  std::scoped_lock lock(state_mutex_);
  audio_snapshot_provider_ = std::move(provider);
}

bool CaptureEngine::RequestClip() {
  if (!initialized_.load(std::memory_order_acquire)) {
    return false;
  }

  if (saving_.exchange(true, std::memory_order_acq_rel)) {
    return false;
  }

  JoinThread(save_thread_);
  save_thread_ = std::thread(&CaptureEngine::SaveClipWorker, this, BuildOutputPath(), 60.0);
  return true;
}

bool CaptureEngine::EnableMultithreadProtection() {
  winrt::com_ptr<ID3D11Multithread> multithread;
  if (FAILED(context_->QueryInterface(IID_PPV_ARGS(multithread.put())))) {
    return false;
  }

  multithread->SetMultithreadProtected(TRUE);
  return true;
}

bool CaptureEngine::QueryAdapterVendorId() {
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

bool CaptureEngine::CreateWgcDevice() {
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

bool CaptureEngine::CreateFence() {
  return SUCCEEDED(device5_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.put())));
}

bool CaptureEngine::CreateHwDeviceContext() {
  CleanupHwDevice();

  hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (hw_device_ctx_ == nullptr) {
    return false;
  }

  auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx_->data);
  auto* d3d11va_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
  d3d11va_ctx->device = device_.get();
  d3d11va_ctx->device->AddRef();
  d3d11va_ctx->device_context = context_.get();
  d3d11va_ctx->device_context->AddRef();

  return av_hwdevice_ctx_init(hw_device_ctx_) >= 0;
}

void CaptureEngine::CleanupHwDevice() {
  if (hw_frames_ctx_ != nullptr) {
    av_buffer_unref(&hw_frames_ctx_);
  }
  if (hw_device_ctx_ != nullptr) {
    av_buffer_unref(&hw_device_ctx_);
  }
}

bool CaptureEngine::CreateHwFramesContextLocked(uint32_t width, uint32_t height) {
  if (hw_frames_ctx_ != nullptr) {
    av_buffer_unref(&hw_frames_ctx_);
  }

  hw_frames_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
  if (hw_frames_ctx_ == nullptr) {
    return false;
  }

  auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx_->data);
  frames_ctx->format = AV_PIX_FMT_D3D11;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
  frames_ctx->width = static_cast<int>(width);
  frames_ctx->height = static_cast<int>(height);
  frames_ctx->initial_pool_size = 0;

  auto* d3d11_frames_ctx = reinterpret_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);
  d3d11_frames_ctx->BindFlags = D3D11_BIND_SHADER_RESOURCE;

  const int init_result = av_hwframe_ctx_init(hw_frames_ctx_);
  if (init_result < 0) {
    AppendLog("av_hwframe_ctx_init failed: " + AvErrorToString(init_result));
    av_buffer_unref(&hw_frames_ctx_);
    return false;
  }

  return true;
}

bool CaptureEngine::SupportsD3D11Frames(const AVCodec* codec) const {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
    if (config == nullptr) {
      break;
    }

    if (config->device_type != AV_HWDEVICE_TYPE_D3D11VA) {
      continue;
    }

    const bool supports_device =
        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0;
    const bool supports_frames =
        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) != 0;
    if (supports_device || supports_frames) {
      return true;
    }
  }

  return false;
}

std::vector<std::string> CaptureEngine::BuildCodecPreference() const {
  constexpr uint32_t kVendorNvidia = 0x10DE;
  constexpr uint32_t kVendorAmd = 0x1002;
  constexpr uint32_t kVendorIntel = 0x8086;

  std::vector<std::string> preferred;
  switch (adapter_vendor_id_) {
    case kVendorNvidia:
      preferred = {"h264_nvenc", "hevc_nvenc", "h264_amf", "hevc_amf", "h264_qsv", "hevc_qsv"};
      break;
    case kVendorAmd:
      preferred = {"h264_amf", "hevc_amf", "h264_nvenc", "hevc_nvenc", "h264_qsv", "hevc_qsv"};
      break;
    case kVendorIntel:
      preferred = {"h264_qsv", "hevc_qsv", "h264_nvenc", "hevc_nvenc", "h264_amf", "hevc_amf"};
      break;
    default:
      preferred = {"h264_nvenc", "hevc_nvenc", "h264_amf", "hevc_amf", "h264_qsv", "hevc_qsv"};
      break;
  }

  preferred.emplace_back("h264_mf");
  preferred.emplace_back("hevc_mf");
  return preferred;
}

bool CaptureEngine::StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

void CaptureEngine::SetEncoderOptions(AVDictionary** options,
                                      const std::string& codec_name,
                                      uint32_t fps) {
  av_dict_set(options, "g", std::to_string(fps * 2).c_str(), 0);
  av_dict_set(options, "bf", "0", 0);

  if (StartsWith(codec_name, "h264_nvenc") || StartsWith(codec_name, "hevc_nvenc")) {
    av_dict_set(options, "preset", "p5", 0);
    av_dict_set(options, "tune", "ll", 0);
    av_dict_set(options, "rc", "cbr_ld_hq", 0);
  } else if (StartsWith(codec_name, "h264_amf") || StartsWith(codec_name, "hevc_amf")) {
    av_dict_set(options, "usage", "ultralowlatency", 0);
    av_dict_set(options, "quality", "speed", 0);
    av_dict_set(options, "rc", "cbr", 0);
  } else if (StartsWith(codec_name, "h264_qsv") || StartsWith(codec_name, "hevc_qsv")) {
    av_dict_set(options, "preset", "veryfast", 0);
    av_dict_set(options, "look_ahead", "0", 0);
    av_dict_set(options, "low_delay_brc", "1", 0);
  } else if (StartsWith(codec_name, "h264_mf") || StartsWith(codec_name, "hevc_mf")) {
    av_dict_set(options, "hw_encoding", "1", 0);
  }
}

bool CaptureEngine::OpenEncoderLocked(const std::string& codec_name, uint32_t width, uint32_t height) {
  const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
  if (codec == nullptr) {
    return false;
  }

  AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
  if (codec_ctx == nullptr) {
    return false;
  }

  codec_ctx->width = static_cast<int>(width);
  codec_ctx->height = static_cast<int>(height);
  codec_ctx->pix_fmt = AV_PIX_FMT_D3D11;
  codec_ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
  codec_ctx->time_base = AVRational{1, 10'000'000};
  codec_ctx->pkt_timebase = codec_ctx->time_base;
  codec_ctx->framerate = AVRational{static_cast<int>(fps_), 1};
  codec_ctx->gop_size = static_cast<int>(fps_ * 2);
  codec_ctx->max_b_frames = 0;
  codec_ctx->thread_count = 1;
  codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  codec_ctx->bit_rate = 12'000'000;
  codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
  if (hw_frames_ctx_ != nullptr) {
    codec_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
  }

  AVDictionary* options = nullptr;
  SetEncoderOptions(&options, codec_name, fps_);

  const int open_result = avcodec_open2(codec_ctx, codec, &options);
  av_dict_free(&options);
  if (open_result < 0) {
    AppendLog("Encoder rejected the requested configuration: " + codec_name + " (" +
              AvErrorToString(open_result) + ")");
    avcodec_free_context(&codec_ctx);
    return false;
  }

  if (codec_ctx_ != nullptr) {
    avcodec_free_context(&codec_ctx_);
  }

  codec_ctx_ = codec_ctx;
  encoder_width_ = width;
  encoder_height_ = height;
  last_packet_pts_ = AV_NOPTS_VALUE;
  last_packet_dts_ = AV_NOPTS_VALUE;

  {
    std::scoped_lock lock(state_mutex_);
    selected_codec_name_ = codec_name;
  }

  AppendLog("Opened encoder: " + codec_name);
  return true;
}

bool CaptureEngine::EnsureEncoderLocked(uint32_t width, uint32_t height) {
  if (codec_ctx_ != nullptr && width == encoder_width_ && height == encoder_height_) {
    return true;
  }

  FlushEncoderLocked();
  ReleaseEncoderLocked();
  packet_buffer_.Clear();
  buffered_.store(false, std::memory_order_release);

  const bool has_hw_frames_ctx = CreateHwFramesContextLocked(width, height);
  if (!has_hw_frames_ctx) {
    AppendLog("Failed to create a hardware frames context; retrying with direct D3D11 texture input.");
  }

  for (const auto& codec_name : BuildCodecPreference()) {
    if (OpenEncoderLocked(codec_name, width, height)) {
      ClearError();
      return true;
    }
  }

  SetError(has_hw_frames_ctx
               ? "No compatible hardware encoder opened with D3D11 zero-copy input."
               : "No compatible hardware encoder accepted direct D3D11 texture input.");
  return false;
}

void CaptureEngine::FlushEncoderLocked() {
  if (codec_ctx_ == nullptr) {
    return;
  }

  avcodec_send_frame(codec_ctx_, nullptr);
  DrainPacketsLocked(last_packet_pts_ == AV_NOPTS_VALUE ? 0 : last_packet_pts_);
}

void CaptureEngine::ReleaseEncoderLocked() {
  if (codec_ctx_ != nullptr) {
    avcodec_free_context(&codec_ctx_);
    codec_ctx_ = nullptr;
  }

  if (hw_frames_ctx_ != nullptr) {
    av_buffer_unref(&hw_frames_ctx_);
  }

  encoder_width_ = 0;
  encoder_height_ = 0;

  {
    std::scoped_lock lock(state_mutex_);
    selected_codec_name_.clear();
  }
}

void CaptureEngine::ReleaseTexture(void* opaque, uint8_t* /*data*/) {
  auto* cookie = reinterpret_cast<Nv12RecycleCookie*>(opaque);
  if (cookie == nullptr || cookie->texture == nullptr) {
    delete cookie;
    return;
  }

  if (cookie->engine != nullptr) {
    cookie->engine->RecycleNv12Texture(cookie->texture, cookie->width, cookie->height);
  } else {
    cookie->texture->Release();
  }
  delete cookie;
}

bool CaptureEngine::EncodeTextureLocked(ID3D11Texture2D* nv12_texture, int64_t pts_100ns) {
  if (codec_ctx_ == nullptr || nv12_texture == nullptr) {
    return false;
  }

  AVFrame* frame = av_frame_alloc();
  if (frame == nullptr) {
    return false;
  }

  frame->format = AV_PIX_FMT_D3D11;
  frame->width = static_cast<int>(encoder_width_);
  frame->height = static_cast<int>(encoder_height_);
  frame->pts = pts_100ns;
  if (hw_frames_ctx_ != nullptr) {
    frame->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
  }

  nv12_texture->AddRef();
  auto* recycle_cookie = new Nv12RecycleCookie{this, nv12_texture, encoder_width_, encoder_height_};
  frame->data[0] = reinterpret_cast<uint8_t*>(nv12_texture);
  frame->data[1] = reinterpret_cast<uint8_t*>(static_cast<intptr_t>(0));
  frame->buf[0] =
      av_buffer_create(reinterpret_cast<uint8_t*>(nv12_texture),
                       sizeof(ID3D11Texture2D*),
                       &CaptureEngine::ReleaseTexture,
                       recycle_cookie,
                       0);

  if (frame->buf[0] == nullptr) {
    delete recycle_cookie;
    nv12_texture->Release();
    av_frame_free(&frame);
    return false;
  }

  int send_result = avcodec_send_frame(codec_ctx_, frame);
  if (send_result == AVERROR(EAGAIN)) {
    DrainPacketsLocked(pts_100ns);
    send_result = avcodec_send_frame(codec_ctx_, frame);
  }

  av_frame_free(&frame);
  if (send_result < 0) {
    SetError("avcodec_send_frame() rejected the D3D11 texture.");
    AppendLog("avcodec_send_frame failed: " + AvErrorToString(send_result));
    return false;
  }

  DrainPacketsLocked(pts_100ns);
  buffered_.store(!packet_buffer_.empty(), std::memory_order_release);
  return true;
}

void CaptureEngine::NormalizePacketTimestamps(AVPacket* packet, int64_t fallback_pts_100ns) {
  const int64_t nominal_duration = std::max<int64_t>(1, 10'000'000 / static_cast<int64_t>(fps_));

  int64_t pts = packet->pts;
  if (pts == AV_NOPTS_VALUE) {
    pts = fallback_pts_100ns;
  }
  if (last_packet_pts_ != AV_NOPTS_VALUE && pts <= last_packet_pts_) {
    pts = last_packet_pts_ + nominal_duration;
  }

  int64_t dts = packet->dts;
  if (dts == AV_NOPTS_VALUE) {
    dts = pts;
  }
  if (last_packet_dts_ != AV_NOPTS_VALUE && dts <= last_packet_dts_) {
    dts = last_packet_dts_ + nominal_duration;
  }
  if (dts > pts) {
    pts = dts;
  }

  packet->pts = pts;
  packet->dts = dts;
  if (packet->duration <= 0) {
    packet->duration = nominal_duration;
  }

  last_packet_pts_ = packet->pts;
  last_packet_dts_ = packet->dts;
}

void CaptureEngine::DrainPacketsLocked(int64_t fallback_pts_100ns) {
  if (codec_ctx_ == nullptr) {
    return;
  }

  AvPacketPtr packet(av_packet_alloc());
  if (!packet) {
    return;
  }

  for (;;) {
    const int receive_result = avcodec_receive_packet(codec_ctx_, packet.get());
    if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
      break;
    }
    if (receive_result < 0) {
      SetError("avcodec_receive_packet() failed while draining the hardware encoder.");
      break;
    }

    NormalizePacketTimestamps(packet.get(), fallback_pts_100ns);
    packet_buffer_.Push(packet.get(), PacketKind::kVideo);
    av_packet_unref(packet.get());
  }
}
bool CaptureEngine::IsWindowCloaked(HWND hwnd) {
  DWORD cloaked = 0;
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
    return false;
  }
  return cloaked != 0;
}

bool CaptureEngine::IsWindowCandidate(HWND hwnd) const {
  if (hwnd == nullptr || hwnd == ui_hwnd_ || !IsWindow(hwnd)) {
    return false;
  }

  if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsWindowCloaked(hwnd)) {
    return false;
  }

  return true;
}

CaptureEngine::CaptureTarget CaptureEngine::DeterminePreferredTarget() {
  if (target_mode_.load(std::memory_order_acquire) == TargetMode::kFullDesktop) {
    return CaptureTarget{CaptureKind::kMonitor, nullptr, MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)};
  }

  const HWND foreground = GetForegroundWindow();
  if (IsWindowCandidate(foreground)) {
    last_window_target_ = foreground;
    return CaptureTarget{CaptureKind::kWindow, foreground, nullptr};
  }

  if (IsWindowCandidate(last_window_target_)) {
    return CaptureTarget{CaptureKind::kWindow, last_window_target_, nullptr};
  }

  return CaptureTarget{CaptureKind::kMonitor, nullptr, MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)};
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureEngine::CreateItemForWindow(HWND hwnd) {
  auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                               IGraphicsCaptureItemInterop>();

  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
  winrt::check_hresult(interop->CreateForWindow(
      hwnd,
      __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
      winrt::put_abi(item)));
  return item;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureEngine::CreateItemForMonitor(HMONITOR monitor) {
  auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                                               IGraphicsCaptureItemInterop>();

  winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
  winrt::check_hresult(interop->CreateForMonitor(
      monitor,
      __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
      winrt::put_abi(item)));
  return item;
}

bool CaptureEngine::StartCaptureForTargetLocked(const CaptureTarget& target) {
  StopCaptureSessionLocked();

  try {
    item_ = (target.kind == CaptureKind::kWindow) ? CreateItemForWindow(target.hwnd)
                                                  : CreateItemForMonitor(target.monitor);
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

  frame_arrived_token_ = frame_pool_.FrameArrived({this, &CaptureEngine::OnFrameArrived});
  session_ = frame_pool_.CreateCaptureSession(item_);
  session_.StartCapture();

  active_target_ = target;
  return EnsureVideoProcessor(current_width_, current_height_);
}

void CaptureEngine::StopCaptureSessionLocked() {
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

void CaptureEngine::CleanupCaptureSession() {
  std::scoped_lock lock(session_mutex_);
  StopCaptureSessionLocked();
}

bool CaptureEngine::EnsureVideoProcessor(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return false;
  }

  if (vp_enumerator_ != nullptr && video_processor_ != nullptr &&
      width == vp_width_ && height == vp_height_) {
    return true;
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
  desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  desc.InputWidth = width;
  desc.InputHeight = height;
  desc.OutputWidth = width;
  desc.OutputHeight = height;
  desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  vp_enumerator_ = nullptr;
  video_processor_ = nullptr;
  if (FAILED(video_device_->CreateVideoProcessorEnumerator(&desc, vp_enumerator_.put()))) {
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

bool CaptureEngine::RecreateNv12Pool(uint32_t width, uint32_t height) {
  std::scoped_lock lock(nv12_pool_mutex_);
  if (nv12_pool_width_ != width || nv12_pool_height_ != height) {
    nv12_pool_.clear();
    nv12_pool_width_ = width;
    nv12_pool_height_ = height;
  }

  if (!nv12_pool_.empty()) {
    return true;
  }

  auto texture = CreateNv12Texture(width, height);
  if (texture == nullptr) {
    return false;
  }

  nv12_pool_.push_back(std::move(texture));
  return true;
}

winrt::com_ptr<ID3D11Texture2D> CaptureEngine::CreateNv12Texture(uint32_t width, uint32_t height) const {
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

  winrt::com_ptr<ID3D11Texture2D> texture;
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, texture.put()))) {
    return nullptr;
  }

  return texture;
}

winrt::com_ptr<ID3D11Texture2D> CaptureEngine::AcquireNv12Texture(uint32_t width, uint32_t height) {
  {
    std::scoped_lock lock(nv12_pool_mutex_);
    if (nv12_pool_width_ == width && nv12_pool_height_ == height && !nv12_pool_.empty()) {
      auto texture = std::move(nv12_pool_.back());
      nv12_pool_.pop_back();
      return texture;
    }
  }

  return CreateNv12Texture(width, height);
}

void CaptureEngine::RecycleNv12Texture(ID3D11Texture2D* texture, uint32_t width, uint32_t height) {
  if (texture == nullptr) {
    return;
  }

  std::scoped_lock lock(nv12_pool_mutex_);
  if (width == nv12_pool_width_ && height == nv12_pool_height_) {
    winrt::com_ptr<ID3D11Texture2D> recycled;
    recycled.attach(texture);
    nv12_pool_.push_back(std::move(recycled));
    return;
  }

  texture->Release();
}

winrt::com_ptr<ID3D11Texture2D> CaptureEngine::AcquireTextureFromSurface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) const {
  auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  winrt::com_ptr<ID3D11Texture2D> texture;
  if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()))) {
    return nullptr;
  }
  return texture;
}

int64_t CaptureEngine::QueryTimestamp100ns() const {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  const int64_t elapsed = counter.QuadPart - qpc_origin_;
  return (elapsed * 10'000'000LL) / qpc_frequency_;
}

bool CaptureEngine::ConvertFrameToNv12(const RawFrame& raw, ConvertedFrame& converted) {
  if (!EnsureVideoProcessor(raw.width, raw.height)) {
    return false;
  }

  auto output_texture = AcquireNv12Texture(raw.width, raw.height);
  if (output_texture == nullptr) {
    SetError("Failed to allocate an NV12 encoder input texture.");
    return false;
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.ArraySlice = 0;

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
  output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_desc.Texture2D.MipSlice = 0;

  winrt::com_ptr<ID3D11VideoProcessorInputView> input_view;
  winrt::com_ptr<ID3D11VideoProcessorOutputView> output_view;

  if (FAILED(video_device_->CreateVideoProcessorInputView(
          raw.bgra_texture.get(), vp_enumerator_.get(), &input_desc, input_view.put()))) {
    return false;
  }

  if (FAILED(video_device_->CreateVideoProcessorOutputView(
          output_texture.get(), vp_enumerator_.get(), &output_desc, output_view.put()))) {
    return false;
  }

  RECT rect{0, 0, static_cast<LONG>(raw.width), static_cast<LONG>(raw.height)};
  video_context_->VideoProcessorSetOutputTargetRect(video_processor_.get(), TRUE, &rect);
  video_context_->VideoProcessorSetStreamSourceRect(video_processor_.get(), 0, TRUE, &rect);
  video_context_->VideoProcessorSetStreamDestRect(video_processor_.get(), 0, TRUE, &rect);
  video_context_->VideoProcessorSetStreamFrameFormat(
      video_processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view.get();

  if (FAILED(video_context_->VideoProcessorBlt(
          video_processor_.get(), output_view.get(), 0, 1, &stream))) {
    return false;
  }

  const uint64_t fence_value = fence_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (FAILED(context4_->Signal(fence_.get(), fence_value))) {
    return false;
  }

  converted.nv12_texture = std::move(output_texture);
  converted.width = raw.width;
  converted.height = raw.height;
  converted.pts_100ns = raw.pts_100ns;
  converted.fence_value = fence_value;
  return true;
}

void CaptureEngine::OnFrameArrived(
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
    winrt::Windows::Foundation::IInspectable const&) {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  std::scoped_lock lock(session_mutex_);
  if (!running_.load(std::memory_order_relaxed) || sender == nullptr) {
    return;
  }

  auto frame = sender.TryGetNextFrame();
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

  RawFrame raw_frame{};
  raw_frame.bgra_texture = std::move(source_texture);
  raw_frame.width = width;
  raw_frame.height = height;
  raw_frame.pts_100ns = QueryTimestamp100ns();

  raw_queue_.try_push(std::move(raw_frame));
}

void CaptureEngine::ProcessingLoop() {
  using namespace std::chrono_literals;

  while (running_.load(std::memory_order_acquire)) {
    RawFrame raw_frame;
    if (!raw_queue_.try_pop(raw_frame)) {
      std::this_thread::sleep_for(1ms);
      continue;
    }

    ConvertedFrame converted;
    if (!ConvertFrameToNv12(raw_frame, converted)) {
      continue;
    }

    if (!encode_queue_.try_push(std::move(converted))) {
      // Encoder is behind. Drop instead of pushing latency back into capture.
    }
  }
}

void CaptureEngine::EncodeLoop() {
  using namespace std::chrono_literals;

  while (running_.load(std::memory_order_acquire)) {
    ConvertedFrame converted;
    if (!encode_queue_.try_pop(converted)) {
      std::this_thread::sleep_for(1ms);
      continue;
    }

    if (FAILED(fence_->SetEventOnCompletion(converted.fence_value, fence_event_))) {
      SetError("Failed to wait on the D3D11 fence.");
      continue;
    }

    if (WaitForSingleObject(fence_event_, 1000) != WAIT_OBJECT_0) {
      SetError("Timed out waiting for the GPU fence before encoding.");
      continue;
    }

    std::scoped_lock lock(encoder_mutex_);
    if (!EnsureEncoderLocked(converted.width, converted.height)) {
      continue;
    }

    EncodeTextureLocked(converted.nv12_texture.get(), converted.pts_100ns);
  }
}

void CaptureEngine::TargetLoop() {
  using namespace std::chrono_literals;

  while (running_.load(std::memory_order_acquire)) {
    const CaptureTarget preferred_target = DeterminePreferredTarget();
    const bool is_dirty = target_dirty_.exchange(false, std::memory_order_acq_rel);

    if (is_dirty || preferred_target != active_target_) {
      std::scoped_lock lock(session_mutex_);
      CaptureTarget start_target = preferred_target;
      if (!StartCaptureForTargetLocked(start_target)) {
        const CaptureTarget fallback{
            CaptureKind::kMonitor, nullptr, MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)};
        if (!StartCaptureForTargetLocked(fallback)) {
          SetError("Failed to start Windows Graphics Capture for the selected target.");
        } else {
          active_target_ = fallback;
          AppendLog("Capture target switched to full desktop.");
          ClearError();
        }
      } else {
        active_target_ = start_target;
        AppendLog(start_target.kind == CaptureKind::kWindow
                      ? "Capture target switched to active window."
                      : "Capture target switched to full desktop.");
        ClearError();
      }
    }

    std::this_thread::sleep_for(250ms);
  }
}

bool CaptureEngine::CopyCodecSnapshot(CodecSnapshot& snapshot) {
  std::scoped_lock lock(encoder_mutex_);
  if (codec_ctx_ == nullptr) {
    return false;
  }

  snapshot.params = avcodec_parameters_alloc();
  if (snapshot.params == nullptr) {
    return false;
  }

  if (avcodec_parameters_from_context(snapshot.params, codec_ctx_) < 0) {
    avcodec_parameters_free(&snapshot.params);
    return false;
  }

  snapshot.time_base = codec_ctx_->time_base;
  return true;
}

bool CaptureEngine::CopyAudioCodecSnapshot(CodecSnapshot& snapshot) const {
  std::function<bool(CodecSnapshot&)> provider;
  {
    std::scoped_lock lock(state_mutex_);
    provider = audio_snapshot_provider_;
  }

  return provider != nullptr ? provider(snapshot) : false;
}

std::wstring CaptureEngine::BuildOutputPath() const {
  SYSTEMTIME local_time{};
  GetLocalTime(&local_time);

  wchar_t filename[128]{};
  swprintf_s(filename,
             L"clip_%04u%02u%02u_%02u%02u%02u_%03u.mp4",
             local_time.wYear,
             local_time.wMonth,
             local_time.wDay,
             local_time.wHour,
             local_time.wMinute,
             local_time.wSecond,
             local_time.wMilliseconds);

  return (clips_directory_ / filename).wstring();
}

std::string CaptureEngine::WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int required =
      WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }

  std::string utf8(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), required, nullptr, nullptr);
  if (!utf8.empty() && utf8.back() == '\0') {
    utf8.pop_back();
  }
  return utf8;
}

bool CaptureEngine::WriteMp4File(const std::wstring& path,
                                 const std::vector<BufferedPacket>& packets,
                                 const CodecSnapshot& video_snapshot,
                                 const CodecSnapshot* audio_snapshot) {
  if (video_snapshot.params == nullptr || packets.empty()) {
    return false;
  }

  const std::string utf8_path = WideToUtf8(path);
  if (utf8_path.empty()) {
    return false;
  }

  AVFormatContext* format_ctx = nullptr;
  if (avformat_alloc_output_context2(&format_ctx, nullptr, "mp4", utf8_path.c_str()) < 0 ||
      format_ctx == nullptr) {
    return false;
  }

  bool ok = false;
  AVStream* video_stream = nullptr;
  AVStream* audio_stream = nullptr;
  constexpr AVRational kCommonPacketTimeBase{1, 10'000'000};
  int64_t base_ts_100ns = AV_NOPTS_VALUE;

  video_stream = avformat_new_stream(format_ctx, nullptr);
  if (video_stream == nullptr) {
    goto cleanup;
  }

  if (avcodec_parameters_copy(video_stream->codecpar, video_snapshot.params) < 0) {
    goto cleanup;
  }
  video_stream->codecpar->codec_tag = 0;
  video_stream->time_base = video_snapshot.time_base;

  if (audio_snapshot != nullptr && audio_snapshot->params != nullptr) {
    audio_stream = avformat_new_stream(format_ctx, nullptr);
    if (audio_stream == nullptr) {
      goto cleanup;
    }

    if (avcodec_parameters_copy(audio_stream->codecpar, audio_snapshot->params) < 0) {
      goto cleanup;
    }
    audio_stream->codecpar->codec_tag = 0;
    audio_stream->time_base = audio_snapshot->time_base;
  }

  if ((format_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
    if (avio_open(&format_ctx->pb, utf8_path.c_str(), AVIO_FLAG_WRITE) < 0) {
      goto cleanup;
    }
  }

  if (avformat_write_header(format_ctx, nullptr) < 0) {
    goto cleanup;
  }

  for (const auto& buffered : packets) {
    if (!buffered.packet) {
      continue;
    }

    if (buffered.kind == PacketKind::kAudio && audio_stream == nullptr) {
      continue;
    }

    AVRational source_time_base = video_snapshot.time_base;
    if (buffered.kind == PacketKind::kAudio) {
      if (audio_snapshot == nullptr) {
        continue;
      }
      source_time_base = audio_snapshot->time_base;
    }

    const AVPacket* packet = buffered.packet.get();
    const int64_t candidate =
        packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
    if (candidate == AV_NOPTS_VALUE) {
      continue;
    }

    const int64_t candidate_100ns =
        av_rescale_q(candidate, source_time_base, kCommonPacketTimeBase);
    if (base_ts_100ns == AV_NOPTS_VALUE || candidate_100ns < base_ts_100ns) {
      base_ts_100ns = candidate_100ns;
    }
  }
  if (base_ts_100ns == AV_NOPTS_VALUE) {
    base_ts_100ns = 0;
  }

  for (const auto& buffered : packets) {
    if (!buffered.packet) {
      continue;
    }

    AVStream* stream = nullptr;
    AVRational source_time_base{};
    if (buffered.kind == PacketKind::kAudio) {
      if (audio_stream == nullptr || audio_snapshot == nullptr) {
        continue;
      }
      stream = audio_stream;
      source_time_base = audio_snapshot->time_base;
    } else {
      stream = video_stream;
      source_time_base = video_snapshot.time_base;
    }

    AVPacket* out = av_packet_clone(buffered.packet.get());
    if (out == nullptr) {
      continue;
    }

    out->stream_index = stream->index;
    const int64_t base_ts_source =
        av_rescale_q(base_ts_100ns, kCommonPacketTimeBase, source_time_base);
    if (out->pts != AV_NOPTS_VALUE) {
      out->pts = std::max<int64_t>(0, out->pts - base_ts_source);
    }
    if (out->dts != AV_NOPTS_VALUE) {
      out->dts = std::max<int64_t>(0, out->dts - base_ts_source);
    }
    av_packet_rescale_ts(out, source_time_base, stream->time_base);
    if (out->pts == AV_NOPTS_VALUE && out->dts != AV_NOPTS_VALUE) {
      out->pts = out->dts;
    }
    if (out->dts == AV_NOPTS_VALUE && out->pts != AV_NOPTS_VALUE) {
      out->dts = out->pts;
    }
    if (out->dts != AV_NOPTS_VALUE && out->pts != AV_NOPTS_VALUE && out->dts > out->pts) {
      out->dts = out->pts;
    }
    out->pos = -1;

    if (av_interleaved_write_frame(format_ctx, out) < 0) {
      av_packet_free(&out);
      goto cleanup;
    }

    av_packet_free(&out);
  }

  if (av_write_trailer(format_ctx) < 0) {
    goto cleanup;
  }

  ok = true;

cleanup:
  if (format_ctx != nullptr) {
    if ((format_ctx->oformat->flags & AVFMT_NOFILE) == 0 && format_ctx->pb != nullptr) {
      avio_closep(&format_ctx->pb);
    }
    avformat_free_context(format_ctx);
  }

  return ok;
}

void CaptureEngine::SaveClipWorker(std::wstring output_path, double seconds) {
  std::vector<BufferedPacket> packets = packet_buffer_.SnapshotLastWindow(seconds);
  if (packets.empty()) {
    AppendLog("Clip request skipped because the packet ring is empty.");
    SetError("Clip save skipped because the RAM packet buffer is still empty.");
    saving_.store(false, std::memory_order_release);
    return;
  }

  CodecSnapshot snapshot;
  if (!CopyCodecSnapshot(snapshot)) {
    AppendLog("Clip request failed because no encoder snapshot was available.");
    SetError("Clip save failed because the encoder is not initialized yet.");
    saving_.store(false, std::memory_order_release);
    return;
  }

  CodecSnapshot audio_snapshot;
  CodecSnapshot* audio_snapshot_ptr = nullptr;
  if (CopyAudioCodecSnapshot(audio_snapshot)) {
    audio_snapshot_ptr = &audio_snapshot;
  }

  if (!WriteMp4File(output_path, packets, snapshot, audio_snapshot_ptr)) {
    AppendLog("Clip save failed while writing MP4: " + WideToUtf8(output_path));
    SetError("Clip save failed while writing the MP4 container.");
  } else {
    AppendLog("Clip saved: " + WideToUtf8(output_path));
    ClearError();
  }

  saving_.store(false, std::memory_order_release);
}

void CaptureEngine::CleanupFenceObjects() {
  if (fence_event_ != nullptr) {
    CloseHandle(fence_event_);
    fence_event_ = nullptr;
  }
  fence_ = nullptr;
}

void CaptureEngine::CleanupEncoder() {
  std::scoped_lock lock(encoder_mutex_);
  FlushEncoderLocked();
  ReleaseEncoderLocked();
}

void CaptureEngine::JoinThread(std::thread& thread) {
  if (thread.joinable()) {
    thread.join();
  }
}

void CaptureEngine::AppendLog(std::string_view message) const {
  if (log_path_.empty()) {
    return;
  }

  std::scoped_lock lock(log_mutex_);
  std::ofstream log_stream(log_path_, std::ios::app);
  if (!log_stream.is_open()) {
    return;
  }

  log_stream << message << '\n';
}

void CaptureEngine::SetError(std::string message) {
  std::scoped_lock lock(state_mutex_);
  last_error_ = std::move(message);
  AppendLog("Error: " + last_error_);
}

void CaptureEngine::ClearError() {
  std::scoped_lock lock(state_mutex_);
  last_error_.clear();
}

std::string StatusLabel(EngineStatus status) {
  switch (status) {
    case EngineStatus::kIdle:
      return "Idle";
    case EngineStatus::kBuffering:
      return "Buffering";
    case EngineStatus::kSaving:
      return "Saving";
  }

  return "Idle";
}

ImVec4 StatusColor(EngineStatus status) {
  switch (status) {
    case EngineStatus::kIdle:
      return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    case EngineStatus::kBuffering:
      return ImVec4(0.15f, 0.78f, 0.43f, 1.0f);
    case EngineStatus::kSaving:
      return ImVec4(0.96f, 0.69f, 0.17f, 1.0f);
  }

  return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
}

bool CreateRenderTarget(D3DState& d3d) {
  winrt::com_ptr<ID3D11Texture2D> back_buffer;
  if (FAILED(d3d.swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.put())))) {
    return false;
  }

  return SUCCEEDED(
      d3d.device->CreateRenderTargetView(back_buffer.get(), nullptr, d3d.render_target_view.put()));
}

void CleanupRenderTarget(D3DState& d3d) {
  d3d.render_target_view = nullptr;
}

bool CreateDeviceD3D(HWND hwnd, D3DState& d3d) {
  DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
  swap_chain_desc.BufferCount = 2;
  swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.OutputWindow = hwnd;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.Windowed = TRUE;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  constexpr D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };

  D3D_FEATURE_LEVEL created_level{};
  const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,
                                                   D3D_DRIVER_TYPE_HARDWARE,
                                                   nullptr,
                                                   flags,
                                                   levels,
                                                   static_cast<UINT>(std::size(levels)),
                                                   D3D11_SDK_VERSION,
                                                   &swap_chain_desc,
                                                   d3d.swap_chain.put(),
                                                   d3d.device.put(),
                                                   &created_level,
                                                   d3d.context.put());
  if (FAILED(hr)) {
    return false;
  }

  return CreateRenderTarget(d3d);
}

void CleanupDeviceD3D(D3DState& d3d) {
  CleanupRenderTarget(d3d);
  d3d.swap_chain = nullptr;
  d3d.context = nullptr;
  d3d.device = nullptr;
}

void ApplyUiStyle() {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 16.0f;
  style.FrameRounding = 12.0f;
  style.GrabRounding = 12.0f;
  style.ScrollbarRounding = 12.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(16.0f, 16.0f);
  style.ItemSpacing = ImVec2(10.0f, 12.0f);
  style.FramePadding = ImVec2(12.0f, 10.0f);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.10f, 0.98f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.17f, 0.10f, 0.28f, 1.00f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.14f, 0.40f, 1.00f);
  colors[ImGuiCol_Border] = ImVec4(0.34f, 0.21f, 0.50f, 0.45f);
  colors[ImGuiCol_Button] = ImVec4(0.40f, 0.18f, 0.66f, 1.00f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.48f, 0.23f, 0.78f, 1.00f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.33f, 0.15f, 0.56f, 1.00f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.16f, 0.28f, 1.00f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.18f, 0.34f, 1.00f);
  colors[ImGuiCol_Header] = ImVec4(0.19f, 0.15f, 0.30f, 1.00f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.20f, 0.40f, 1.00f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.29f, 0.22f, 0.46f, 1.00f);
  colors[ImGuiCol_PlotHistogram] = ImVec4(0.68f, 0.42f, 1.00f, 1.00f);
  colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.80f, 0.58f, 1.00f, 1.00f);
}

void RenderMainWindow(CaptureEngine& engine, WASAPIAudioCapture& audio, bool hotkey_registered) {
  auto activity_label = [](bool active) { return active ? "Active" : "Idle"; };
  auto activity_color = [](bool active) {
    return active ? ImVec4(0.15f, 0.78f, 0.43f, 1.0f) : ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
  };

  const EngineStatus status = engine.status();
  const ImVec4 status_color = StatusColor(status);
  const std::string error_text = engine.last_error();
  const std::string codec_name = engine.selected_codec();
  const double buffered_seconds = engine.buffered_seconds();
  const bool game_audio_active = audio.game_audio_active();
  const bool microphone_active = audio.microphone_active();
  const float game_audio_level = audio.game_audio_level();
  const float microphone_level = audio.microphone_level();
  const auto microphones = audio.microphones();
  const int selected_microphone_index = audio.selected_microphone_index();

  constexpr ImVec2 window_size(440.0f, 420.0f);
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - window_size.x) * 0.5f,
             viewport->WorkPos.y + (viewport->WorkSize.y - window_size.y) * 0.5f),
      ImGuiCond_Always);
  ImGui::Begin("Klip",
               nullptr,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse);

  ImGui::TextUnformatted("Status");
  ImGui::SameLine();
  ImGui::Dummy(ImVec2(8.0f, 0.0f));
  ImGui::SameLine();
  ImGui::TextColored(status_color, "%s", StatusLabel(status).c_str());

  ImGui::Spacing();

  if (!engine.is_ready() || engine.is_saving()) {
    ImGui::BeginDisabled();
  }

  if (ImGui::Button("SAVE CLIP", ImVec2(-1.0f, 38.0f))) {
    engine.RequestClip();
  }

  if (!engine.is_ready() || engine.is_saving()) {
    ImGui::EndDisabled();
  }

  ImGui::TextDisabled("%s", hotkey_registered ? "Hotkeys: Alt+C save, Alt+X toggle UI"
                                              : "Hotkeys unavailable");

  ImGui::Separator();
  ImGui::TextUnformatted("Game Audio");
  ImGui::SameLine();
  ImGui::Dummy(ImVec2(8.0f, 0.0f));
  ImGui::SameLine();
  ImGui::TextColored(activity_color(game_audio_active), "%s", activity_label(game_audio_active));
  ImGui::ProgressBar(game_audio_level, ImVec2(-1.0f, 8.0f), "");

  ImGui::TextUnformatted("Microphone");
  ImGui::SameLine();
  ImGui::Dummy(ImVec2(8.0f, 0.0f));
  ImGui::SameLine();
  ImGui::TextColored(activity_color(microphone_active), "%s", activity_label(microphone_active));
  ImGui::ProgressBar(microphone_level, ImVec2(-1.0f, 8.0f), "");

  int target_index = engine.target_mode() == TargetMode::kActiveWindow ? 0 : 1;
  constexpr const char* kTargetLabels[] = {"Active Window", "Full Desktop"};
  if (ImGui::Combo("Target", &target_index, kTargetLabels, IM_ARRAYSIZE(kTargetLabels))) {
    engine.SetTargetMode(target_index == 0 ? TargetMode::kActiveWindow : TargetMode::kFullDesktop);
  }

  std::string selected_microphone_label = "No microphone detected";
  if (selected_microphone_index >= 0 &&
      selected_microphone_index < static_cast<int>(microphones.size())) {
    selected_microphone_label = microphones[static_cast<std::size_t>(selected_microphone_index)].display_name;
  }

  if (ImGui::BeginCombo("Microphone", selected_microphone_label.c_str())) {
    for (int index = 0; index < static_cast<int>(microphones.size()); ++index) {
      const bool is_selected = index == selected_microphone_index;
      if (ImGui::Selectable(microphones[static_cast<std::size_t>(index)].display_name.c_str(),
                            is_selected)) {
        audio.SetSelectedMicrophoneIndex(index);
      }
      if (is_selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  if (ImGui::Button("Open Folder", ImVec2(-1.0f, 32.0f))) {
    const std::wstring folder = engine.clips_directory().wstring();
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }

  ImGui::Separator();
  ImGui::Text("Buffered: %.1fs / 60.0s", buffered_seconds);
  ImGui::Text("Packets: %zu", engine.buffered_packets());
  ImGui::Text("Encoder: %s", codec_name.empty() ? "initializing..." : codec_name.c_str());

  if (!error_text.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("Last issue: %s", error_text.c_str());
  }

  ImGui::End();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w_param, l_param)) {
    return 1;
  }

  auto* app = reinterpret_cast<AppContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_SIZE:
      if (app != nullptr && app->d3d != nullptr && w_param != SIZE_MINIMIZED) {
        app->d3d->pending_width = static_cast<UINT>(LOWORD(l_param));
        app->d3d->pending_height = static_cast<UINT>(HIWORD(l_param));
      }
      return 0;

    case WM_HOTKEY:
      if (app != nullptr) {
        if (app->engine != nullptr && w_param == 1) {
          app->engine->RequestClip();
          return 0;
        }
        if (app->hwnd != nullptr && w_param == 2) {
          app->ui_visible = !app->ui_visible;
          if (app->ui_visible) {
            ShowWindow(app->hwnd, SW_SHOW);
            SetForegroundWindow(app->hwnd);
          } else {
            ShowWindow(app->hwnd, SW_HIDE);
          }
          return 0;
        }
      }
      break;

    case WM_SYSCOMMAND:
      if ((w_param & 0xfff0) == SC_KEYMENU) {
        return 0;
      }
      break;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }

  return DefWindowProcW(hwnd, msg, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  av_log_set_level(AV_LOG_ERROR);

  ImGui_ImplWin32_EnableDpiAwareness();

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_CLASSDC;
  window_class.lpfnWndProc = WndProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = L"KlipWindowClass";
  RegisterClassExW(&window_class);

  constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT window_rect{0, 0, 470, 380};
  AdjustWindowRect(&window_rect, window_style, FALSE);

  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor_info);
  const int window_width = window_rect.right - window_rect.left;
  const int window_height = window_rect.bottom - window_rect.top;
  const int window_x =
      monitor_info.rcWork.left + ((monitor_info.rcWork.right - monitor_info.rcWork.left) - window_width) / 2;
  const int window_y =
      monitor_info.rcWork.top + ((monitor_info.rcWork.bottom - monitor_info.rcWork.top) - window_height) / 2;

  HWND hwnd = CreateWindowW(window_class.lpszClassName,
                            L"Klip",
                            window_style,
                            window_x,
                            window_y,
                            window_width,
                            window_height,
                            nullptr,
                            nullptr,
                            instance,
                            nullptr);

  if (hwnd == nullptr) {
    UnregisterClassW(window_class.lpszClassName, instance);
    return 1;
  }

  D3DState d3d;
  if (!CreateDeviceD3D(hwnd, d3d)) {
    DestroyWindow(hwnd);
    UnregisterClassW(window_class.lpszClassName, instance);
    return 1;
  }

  ShowWindow(hwnd, show_command);
  UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ApplyUiStyle();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(d3d.device.get(), d3d.context.get());

  const std::filesystem::path clips_directory = std::filesystem::current_path() / "clips";

  CaptureEngine engine;
  engine.Initialize(d3d.device.get(), d3d.context.get(), hwnd, clips_directory);
  WASAPIAudioCapture audio;
  if (engine.is_ready()) {
    audio.Initialize(engine.packet_buffer(), engine.qpc_origin(), engine.qpc_frequency());
  }
  engine.SetAudioSnapshotProvider([&audio](CodecSnapshot& snapshot) {
    return audio.CopyCodecSnapshot(snapshot);
  });

  AppContext app_context{
      .d3d = &d3d,
      .engine = &engine,
      .hwnd = hwnd,
      .save_hotkey_registered = RegisterHotKey(hwnd, 1, MOD_ALT | MOD_NOREPEAT, 'C') == TRUE,
      .toggle_hotkey_registered = RegisterHotKey(hwnd, 2, MOD_ALT | MOD_NOREPEAT, 'X') == TRUE,
      .ui_visible = true,
  };
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app_context));

  bool done = false;
  while (!done) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) {
        done = true;
      }
    }

    if (done) {
      break;
    }

    if (!app_context.ui_visible) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      continue;
    }

    if (d3d.pending_width != 0 && d3d.pending_height != 0) {
      CleanupRenderTarget(d3d);
      d3d.swap_chain->ResizeBuffers(0, d3d.pending_width, d3d.pending_height, DXGI_FORMAT_UNKNOWN, 0);
      d3d.pending_width = 0;
      d3d.pending_height = 0;
      CreateRenderTarget(d3d);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RenderMainWindow(engine, audio, app_context.save_hotkey_registered);

    ImGui::Render();
    constexpr float clear_color[4] = {0.06f, 0.07f, 0.09f, 1.00f};
    ID3D11RenderTargetView* render_target = d3d.render_target_view.get();
    d3d.context->OMSetRenderTargets(1, &render_target, nullptr);
    d3d.context->ClearRenderTargetView(d3d.render_target_view.get(), clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    d3d.swap_chain->Present(1, 0);
  }

  if (app_context.save_hotkey_registered) {
    UnregisterHotKey(hwnd, 1);
  }
  if (app_context.toggle_hotkey_registered) {
    UnregisterHotKey(hwnd, 2);
  }

  engine.SetAudioSnapshotProvider({});
  engine.Shutdown();
  audio.Shutdown();

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  CleanupDeviceD3D(d3d);
  DestroyWindow(hwnd);
  UnregisterClassW(window_class.lpszClassName, instance);
  return 0;
}
