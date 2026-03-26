#include "async_mp4_writer.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <cwchar>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace klip {

namespace {

std::string WideToUtf8(const std::wstring& value) {
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

}  // namespace

AsyncMp4Writer::AsyncMp4Writer() = default;

AsyncMp4Writer::~AsyncMp4Writer() {
  Stop();
}

bool AsyncMp4Writer::Start(const ReplayRingBuffer* ring_buffer, const EncoderBackend* encoder) {
  ring_buffer_ = ring_buffer;
  encoder_ = encoder;

  if (ring_buffer_ == nullptr || encoder_ == nullptr) {
    return false;
  }

  if (running_.exchange(true)) {
    return true;
  }

  thread_ = std::thread(&AsyncMp4Writer::ThreadLoop, this);
  return true;
}

void AsyncMp4Writer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (thread_.joinable()) {
    thread_.join();
  }
}

bool AsyncMp4Writer::RequestWrite() {
  Request req{};
  req.output_path = BuildOutputPath();
  req.window_seconds = 60.0;
  return queue_.try_enqueue(std::move(req));
}

std::wstring AsyncMp4Writer::BuildOutputPath() {
  const std::filesystem::path out_dir = std::filesystem::current_path() / "clips";
  std::error_code ec{};
  std::filesystem::create_directories(out_dir, ec);

  SYSTEMTIME st{};
  GetLocalTime(&st);

  wchar_t file_name[128]{};
  swprintf_s(
      file_name,
      L"clip_%04u%02u%02u_%02u%02u%02u.mp4",
      st.wYear,
      st.wMonth,
      st.wDay,
      st.wHour,
      st.wMinute,
      st.wSecond);

  return (out_dir / file_name).wstring();
}

bool AsyncMp4Writer::WriteMp4File(const std::wstring& path,
                                  const std::vector<AvPacketPtr>& packets,
                                  const AVCodecContext* codec_ctx,
                                  AVRational packet_time_base) {
  if (packets.empty() || codec_ctx == nullptr) {
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
  int64_t first_pts = AV_NOPTS_VALUE;
  int64_t first_dts = AV_NOPTS_VALUE;
  AVStream* stream = avformat_new_stream(format_ctx, nullptr);
  if (stream == nullptr) {
    goto cleanup;
  }

  stream->time_base = packet_time_base;
  if (avcodec_parameters_from_context(stream->codecpar, codec_ctx) < 0) {
    goto cleanup;
  }

  stream->codecpar->codec_tag = 0;

  if ((format_ctx->oformat->flags & AVFMT_NOFILE) == 0) {
    if (avio_open(&format_ctx->pb, utf8_path.c_str(), AVIO_FLAG_WRITE) < 0) {
      goto cleanup;
    }
  }

  if (avformat_write_header(format_ctx, nullptr) < 0) {
    goto cleanup;
  }

  for (const auto& packet : packets) {
    if (!packet) {
      continue;
    }

    AVPacket* out = av_packet_alloc();
    if (out == nullptr) {
      continue;
    }

    if (av_packet_ref(out, packet.get()) < 0) {
      av_packet_free(&out);
      continue;
    }

    out->stream_index = stream->index;

    if (first_pts == AV_NOPTS_VALUE && out->pts != AV_NOPTS_VALUE) {
      first_pts = out->pts;
    }
    if (first_dts == AV_NOPTS_VALUE && out->dts != AV_NOPTS_VALUE) {
      first_dts = out->dts;
    }

    if (first_pts != AV_NOPTS_VALUE && out->pts != AV_NOPTS_VALUE) {
      out->pts -= first_pts;
    }
    if (first_dts != AV_NOPTS_VALUE && out->dts != AV_NOPTS_VALUE) {
      out->dts -= first_dts;
    }

    if (out->dts != AV_NOPTS_VALUE && out->pts != AV_NOPTS_VALUE && out->dts > out->pts) {
      out->dts = out->pts;
    }

    if (av_interleaved_write_frame(format_ctx, out) < 0) {
      av_packet_free(&out);
      break;
    }

    av_packet_free(&out);
  }

  av_write_trailer(format_ctx);
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

void AsyncMp4Writer::ThreadLoop() {
  using namespace std::chrono_literals;

  while (running_.load(std::memory_order_acquire)) {
    Request req;
    if (!queue_.try_dequeue(req)) {
      std::this_thread::sleep_for(10ms);
      continue;
    }

    const AVCodecContext* codec_ctx = encoder_->codec_context();
    if (codec_ctx == nullptr) {
      continue;
    }

    std::vector<AvPacketPtr> packets;
    for (int attempt = 0; attempt < 80 && running_.load(std::memory_order_acquire); ++attempt) {
      packets = ring_buffer_->SnapshotLastWindow(req.window_seconds);
      if (!packets.empty()) {
        break;
      }
      std::this_thread::sleep_for(100ms);
    }

    if (packets.empty()) {
      std::wcout << L"Clip save skipped (no encoded packets yet): " << req.output_path << std::endl;
      continue;
    }

    if (WriteMp4File(req.output_path, packets, codec_ctx, ring_buffer_->time_base())) {
      std::wcout << L"Clip saved: " << req.output_path << std::endl;
    } else {
      std::wcout << L"Clip save failed: " << req.output_path << std::endl;
    }
  }
}

}  // namespace klip
