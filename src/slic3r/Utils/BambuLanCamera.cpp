///|/ Copyright (c) 2026 Stan Elston
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BambuLanCamera.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <thread>

#if defined(SLIC3R_BAMBU_CAMERA) && !defined(_WIN32)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}
#endif

namespace Slic3r {
namespace {

#if defined(SLIC3R_BAMBU_CAMERA) && !defined(_WIN32)

std::string percent_encode(const std::string &in)
{
    std::string out;
    out.reserve(in.size() * 3);
    static const char *hex = "0123456789ABCDEF";
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::string camera_host(const std::string &host)
{
    if (host.find(':') == std::string::npos)
        return host;
    return "[" + host + "]";
}

struct DecodeFlag
{
    std::atomic<bool> stop{false};
};

int camera_interrupt(void *opaque)
{
    return static_cast<DecodeFlag *>(opaque)->stop.load() ? 1 : 0;
}

std::string av_error_text(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    buf[0] = 0;
    av_strerror(err, buf, sizeof(buf));
    const std::string text(buf);
    // Never surface a URL. The access code is part of it.
    if (text.find("://") != std::string::npos || text.find("bblp") != std::string::npos)
        return "Could not open the camera stream.";
    return text.empty() ? "Could not open the camera stream." : text;
}

void decode_stream(const std::string &host, const std::string &access_code, DecodeFlag &flag,
                   const BambuLanCamera::FrameFn &on_frame, const BambuLanCamera::ErrorFn &on_error)
{
    av_log_set_level(AV_LOG_QUIET);
    std::string url = "rtsps://bblp:" + percent_encode(access_code) + "@" + camera_host(host) + ":322/streaming/live/1";

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        if (on_error)
            on_error("Could not open the camera stream.");
        url.assign(url.size(), '\0');
        return;
    }
    fmt->interrupt_callback.callback = camera_interrupt;
    fmt->interrupt_callback.opaque = &flag;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "tls_verify", "0", 0);
    const int opened = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    url.assign(url.size(), '\0');
    url.clear();
    if (opened < 0) {
        if (on_error && !flag.stop.load())
            on_error(av_error_text(opened));
        if (fmt)
            avformat_close_input(&fmt);
        return;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        if (on_error && !flag.stop.load())
            on_error("The camera stream has no video.");
        avformat_close_input(&fmt);
        return;
    }

    const int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vindex < 0) {
        if (on_error && !flag.stop.load())
            on_error("The camera stream has no video.");
        avformat_close_input(&fmt);
        return;
    }

    AVStream *stream = fmt->streams[vindex];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext *dec = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!dec || avcodec_parameters_to_context(dec, stream->codecpar) < 0 || avcodec_open2(dec, codec, nullptr) < 0) {
        if (on_error && !flag.stop.load())
            on_error("Could not decode the camera stream.");
        if (dec)
            avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SwsContext *sws = nullptr;
    if (!pkt || !frame) {
        if (on_error && !flag.stop.load())
            on_error("Could not decode the camera stream.");
    }

    while (pkt && frame && !flag.stop.load()) {
        const int read = av_read_frame(fmt, pkt);
        if (read < 0) {
            av_packet_unref(pkt);
            if (read == AVERROR(EAGAIN))
                continue;
            if (read != AVERROR_EXIT && read != AVERROR_EOF && !flag.stop.load() && on_error)
                on_error(av_error_text(read));
            break;
        }
        if (pkt->stream_index == vindex && avcodec_send_packet(dec, pkt) == 0) {
            while (!flag.stop.load() && avcodec_receive_frame(dec, frame) == 0) {
                const int w = frame->width;
                const int h = frame->height;
                if (w <= 0 || h <= 0)
                    continue;
                sws = sws_getCachedContext(sws, w, h, static_cast<AVPixelFormat>(frame->format),
                                           w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws)
                    continue;
                BambuLanCamera::Frame out;
                out.width = w;
                out.height = h;
                out.rgb.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
                uint8_t *dst[4] = { out.rgb.data(), nullptr, nullptr, nullptr };
                int dst_stride[4] = { w * 3, 0, 0, 0 };
                if (sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dst_stride) > 0 && on_frame)
                    on_frame(std::move(out));
            }
        }
        av_packet_unref(pkt);
    }

    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

#endif

} // namespace

struct BambuLanCamera::Impl
{
    std::thread worker;
#if defined(SLIC3R_BAMBU_CAMERA) && !defined(_WIN32)
    DecodeFlag flag;
#endif
};

BambuLanCamera::BambuLanCamera()
    : m_impl(std::make_unique<Impl>())
{
}

BambuLanCamera::~BambuLanCamera()
{
    stop();
}

bool BambuLanCamera::available()
{
#if defined(SLIC3R_BAMBU_CAMERA) && !defined(_WIN32)
    return true;
#else
    return false;
#endif
}

void BambuLanCamera::start(const std::string &host, const std::string &access_code, FrameFn on_frame, ErrorFn on_error)
{
    if (!m_impl)
        return;
    stop();
#if defined(SLIC3R_BAMBU_CAMERA) && !defined(_WIN32)
    m_impl->flag.stop.store(false);
    m_impl->worker = std::thread([this, host, access_code, on_frame = std::move(on_frame), on_error = std::move(on_error)] {
        decode_stream(host, access_code, m_impl->flag, on_frame, on_error);
    });
#else
    (void) host;
    (void) access_code;
    (void) on_frame;
    if (on_error)
        on_error("Video libraries are unavailable.");
#endif
}

void BambuLanCamera::stop()
{
    if (!m_impl)
        return;
#if defined(SLIC3R_BAMBU_CAMERA) && !defined(_WIN32)
    m_impl->flag.stop.store(true);
#endif
    if (m_impl->worker.joinable())
        m_impl->worker.join();
}

} // namespace Slic3r
