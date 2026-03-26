#include "encoder_backend.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <string>

namespace klip {

namespace {

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

EncoderBackend::EncoderBackend() = default;

EncoderBackend::~EncoderBackend() {
  Flush();

  if (codec_ctx_ != nullptr) {
    avcodec_free_context(&codec_ctx_);
  }

  if (hw_frames_ctx_ != nullptr) {
    av_buffer_unref(&hw_frames_ctx_);
  }

  if (hw_device_ctx_ != nullptr) {
    av_buffer_unref(&hw_device_ctx_);
  }
}

bool EncoderBackend::Initialize(ID3D11Device* device,
                                ID3D11DeviceContext* context,
                                uint32_t vendor_id,
                                uint32_t width,
                                uint32_t height,
                                uint32_t fps,
                                ReplayRingBuffer* ring_buffer) {
  device_ = device;
  context_ = context;
  width_ = width;
  height_ = height;
  fps_ = fps;
  ring_buffer_ = ring_buffer;

  if (device_ == nullptr || context_ == nullptr || ring_buffer_ == nullptr) {
    return false;
  }

  if (!CreateHwDeviceContext()) {
    return false;
  }

  const auto candidates = BuildCodecPreference(vendor_id);
  for (const auto& codec_name : candidates) {
    if (OpenEncoder(codec_name)) {
      selected_codec_name_ = codec_name;
      return true;
    }
  }

  return false;
}

bool EncoderBackend::EncodeTexture(ID3D11Texture2D* nv12_texture, int64_t pts_100ns) {
  if (codec_ctx_ == nullptr || nv12_texture == nullptr) {
    return false;
  }

  AVFrame* frame = av_frame_alloc();
  if (frame == nullptr) {
    return false;
  }

  frame->format = AV_PIX_FMT_D3D11;
  frame->width = static_cast<int>(width_);
  frame->height = static_cast<int>(height_);
  frame->pts = pts_100ns;
  if (hw_frames_ctx_ != nullptr) {
    frame->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
  }

  nv12_texture->AddRef();
  frame->data[0] = reinterpret_cast<uint8_t*>(nv12_texture);
  frame->data[1] = reinterpret_cast<uint8_t*>(static_cast<intptr_t>(0));
  frame->buf[0] = av_buffer_create(
      reinterpret_cast<uint8_t*>(nv12_texture),
      sizeof(ID3D11Texture2D*),
      &EncoderBackend::ReleaseD3D11Texture,
      nullptr,
      0);

  if (frame->buf[0] == nullptr) {
    nv12_texture->Release();
    av_frame_free(&frame);
    return false;
  }

  const int send_ret = avcodec_send_frame(codec_ctx_, frame);
  av_frame_free(&frame);

  if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
    return false;
  }

  DrainPackets();
  return true;
}

void EncoderBackend::Flush() {
  if (codec_ctx_ == nullptr) {
    return;
  }

  avcodec_send_frame(codec_ctx_, nullptr);
  DrainPackets();
}

void EncoderBackend::ReleaseD3D11Texture(void* /*opaque*/, uint8_t* data) {
  auto* texture = reinterpret_cast<ID3D11Texture2D*>(data);
  if (texture != nullptr) {
    texture->Release();
  }
}

bool EncoderBackend::CreateHwDeviceContext() {
  if (hw_device_ctx_ != nullptr) {
    av_buffer_unref(&hw_device_ctx_);
  }

  hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (hw_device_ctx_ == nullptr) {
    return false;
  }

  auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx_->data);
  auto* d3d11va_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);

  d3d11va_ctx->device = device_;
  d3d11va_ctx->device->AddRef();

  d3d11va_ctx->device_context = context_;
  d3d11va_ctx->device_context->AddRef();

  return av_hwdevice_ctx_init(hw_device_ctx_) >= 0;
}

bool EncoderBackend::CreateHwFramesContext() {
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
  frames_ctx->width = static_cast<int>(width_);
  frames_ctx->height = static_cast<int>(height_);
  frames_ctx->initial_pool_size = 16;

  auto* d3d11_frames_ctx = reinterpret_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);
  d3d11_frames_ctx->BindFlags = D3D11_BIND_SHADER_RESOURCE;
#ifdef D3D11_BIND_VIDEO_ENCODER
  d3d11_frames_ctx->BindFlags |= D3D11_BIND_VIDEO_ENCODER;
#endif

  return av_hwframe_ctx_init(hw_frames_ctx_) >= 0;
}

bool EncoderBackend::SupportsD3D11Frames(const AVCodec* codec) const {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
    if (cfg == nullptr) {
      break;
    }

    if (cfg->device_type != AV_HWDEVICE_TYPE_D3D11VA) {
      continue;
    }

    const bool has_valid_method =
        (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 ||
        (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) != 0;

    if (has_valid_method) {
      return true;
    }
  }

  return false;
}

bool EncoderBackend::OpenEncoder(const std::string& codec_name) {
  const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
  if (codec == nullptr) {
    return false;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (ctx == nullptr) {
    return false;
  }

  ctx->width = static_cast<int>(width_);
  ctx->height = static_cast<int>(height_);
  ctx->pix_fmt = AV_PIX_FMT_D3D11;
  ctx->time_base = AVRational{1, 10'000'000};
  ctx->pkt_timebase = ctx->time_base;
  ctx->framerate = AVRational{static_cast<int>(fps_), 1};
  ctx->gop_size = static_cast<int>(fps_ * 2);
  ctx->max_b_frames = 0;
  ctx->thread_count = 1;
  ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx->bit_rate = 12'000'000;

  ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
  if (hw_frames_ctx_ != nullptr) {
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
  }

  AVDictionary* opts = nullptr;
  SetCommonRateControlOptions(&opts, codec_name, fps_);

  const int open_result = avcodec_open2(ctx, codec, &opts);
  av_dict_free(&opts);

  if (open_result < 0) {
    avcodec_free_context(&ctx);
    return false;
  }

  if (codec_ctx_ != nullptr) {
    avcodec_free_context(&codec_ctx_);
  }

  codec_ctx_ = ctx;
  return true;
}

void EncoderBackend::DrainPackets() {
  if (codec_ctx_ == nullptr || ring_buffer_ == nullptr) {
    return;
  }

  AvPacketPtr packet(av_packet_alloc());
  if (!packet) {
    return;
  }

  for (;;) {
    const int ret = avcodec_receive_packet(codec_ctx_, packet.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }

    if (ret < 0) {
      break;
    }

    ring_buffer_->Push(packet.get());
  }
}

std::vector<std::string> EncoderBackend::BuildCodecPreference(uint32_t vendor_id) const {
  constexpr uint32_t kVendorNvidia = 0x10DE;
  constexpr uint32_t kVendorAmd = 0x1002;
  constexpr uint32_t kVendorIntel = 0x8086;

  std::vector<std::string> preferred;
  if (vendor_id == kVendorNvidia) {
    preferred = {"h264_nvenc", "hevc_nvenc", "av1_nvenc"};
  } else if (vendor_id == kVendorAmd) {
    preferred = {"h264_amf", "hevc_amf", "av1_amf"};
  } else if (vendor_id == kVendorIntel) {
    preferred = {"h264_mf", "hevc_mf", "h264_qsv", "hevc_qsv", "av1_qsv"};
  }

    const std::array<std::string, 11> fallback = {
      "h264_mf", "hevc_mf", "h264_nvenc", "hevc_nvenc", "av1_nvenc", "h264_amf",
      "hevc_amf", "av1_amf", "h264_qsv",  "hevc_qsv",   "av1_qsv"};

  for (const auto& codec : fallback) {
    if (std::find(preferred.begin(), preferred.end(), codec) == preferred.end()) {
      preferred.push_back(codec);
    }
  }

  return preferred;
}

void EncoderBackend::SetCommonRateControlOptions(AVDictionary** opts,
                                                  const std::string& codec_name,
                                                  uint32_t fps) {
  av_dict_set(opts, "g", std::to_string(fps * 2).c_str(), 0);
  av_dict_set(opts, "bf", "0", 0);

  if (StartsWith(codec_name, "h264_nvenc") || StartsWith(codec_name, "hevc_nvenc")) {
    av_dict_set(opts, "preset", "p5", 0);
    av_dict_set(opts, "tune", "ll", 0);
    av_dict_set(opts, "rc", "cbr_ld_hq", 0);
  } else if (StartsWith(codec_name, "av1_nvenc")) {
    av_dict_set(opts, "preset", "p5", 0);
  } else if (StartsWith(codec_name, "h264_amf") || StartsWith(codec_name, "hevc_amf") ||
             StartsWith(codec_name, "av1_amf")) {
    av_dict_set(opts, "usage", "ultralowlatency", 0);
    av_dict_set(opts, "quality", "speed", 0);
    av_dict_set(opts, "rc", "cbr", 0);
  } else if (StartsWith(codec_name, "h264_qsv") || StartsWith(codec_name, "hevc_qsv") ||
             StartsWith(codec_name, "av1_qsv")) {
    av_dict_set(opts, "preset", "veryfast", 0);
    av_dict_set(opts, "look_ahead", "0", 0);
    av_dict_set(opts, "low_delay_brc", "1", 0);
  } else if (StartsWith(codec_name, "h264_mf") || StartsWith(codec_name, "hevc_mf")) {
    av_dict_set(opts, "hw_encoding", "1", 0);
  }
}

}  // namespace klip