#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>

#include "replay_ring_buffer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace klip {

class EncoderBackend {
 public:
  EncoderBackend();
  ~EncoderBackend();

  EncoderBackend(const EncoderBackend&) = delete;
  EncoderBackend& operator=(const EncoderBackend&) = delete;

  bool Initialize(ID3D11Device* device,
                  ID3D11DeviceContext* context,
                  uint32_t vendor_id,
                  uint32_t width,
                  uint32_t height,
                  uint32_t fps,
                  ReplayRingBuffer* ring_buffer);

  bool EncodeTexture(ID3D11Texture2D* nv12_texture, int64_t pts_100ns);
  void Flush();

  const AVCodecContext* codec_context() const { return codec_ctx_; }
  AVRational time_base() const { return codec_ctx_ != nullptr ? codec_ctx_->time_base : AVRational{1, 10000000}; }
  const std::string& selected_codec_name() const { return selected_codec_name_; }

 private:
  static void ReleaseD3D11Texture(void* opaque, uint8_t* data);

  bool CreateHwDeviceContext();
  bool CreateHwFramesContext();
  bool OpenEncoder(const std::string& codec_name);
  bool SupportsD3D11Frames(const AVCodec* codec) const;
  void DrainPackets();

  std::vector<std::string> BuildCodecPreference(uint32_t vendor_id) const;
  static void SetCommonRateControlOptions(AVDictionary** opts, const std::string& codec_name, uint32_t fps);

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t fps_ = 60;

  ReplayRingBuffer* ring_buffer_ = nullptr;

  AVBufferRef* hw_device_ctx_ = nullptr;
  AVBufferRef* hw_frames_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;

  std::string selected_codec_name_;
};

}  // namespace klip