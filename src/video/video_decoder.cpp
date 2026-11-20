#include "video/video_decoder.h"

#include <cmath>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct VideoDecoder::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int stream = -1;
    AVRational fps{30, 1};
    AVRational timeBase{1, 1};
    int64_t startPts = 0;
    int frameCount = 0;
    int rotation = 0;
    int srcW = 0, srcH = 0;
    int current = -1;
    bool eof = false;  // demuxer exhausted and decoder flushed with null packet
    bool hasFrame = false;
    std::vector<uint8_t> conv, out;

    ~Impl() {
        sws_freeContext(sws);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&codec);
        avformat_close_input(&fmt);
    }

    int frameIndex(const AVFrame* f) const {
        int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = f->pts;
        if (ts == AV_NOPTS_VALUE) return current + 1;
        return (int)std::llround((double)(ts - startPts) * av_q2d(timeBase) * av_q2d(fps));
    }

    // Decodes the next frame into `frame`; returns its index, or -1 on EOF/error.
    int next() {
        for (;;) {
            int r = avcodec_receive_frame(codec, frame);
            if (r == 0) return frameIndex(frame);
            if (r != AVERROR(EAGAIN)) return -1;  // AVERROR_EOF or error
            if (eof) return -1;
            for (;;) {
                r = av_read_frame(fmt, pkt);
                if (r < 0) {
                    eof = true;
                    avcodec_send_packet(codec, nullptr);
                    break;
                }
                bool ours = pkt->stream_index == stream;
                if (ours) r = avcodec_send_packet(codec, pkt);
                av_packet_unref(pkt);
                if (ours) break;
            }
        }
    }

    void convert() {
        uint8_t* dst[4] = {conv.data(), nullptr, nullptr, nullptr};
        int stride[4] = {srcW * 4, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, srcH, dst, stride);
        if (rotation == 0) return;
        const uint32_t* s = reinterpret_cast<const uint32_t*>(conv.data());
        uint32_t* d = reinterpret_cast<uint32_t*>(out.data());
        for (int y = 0; y < srcH; ++y) {
            for (int x = 0; x < srcW; ++x) {
                uint32_t px = s[y * srcW + x];
                if (rotation == 90) d[x * srcH + (srcH - 1 - y)] = px;  // clockwise, out width = srcH
                else if (rotation == 180) d[(srcH - 1 - y) * srcW + (srcW - 1 - x)] = px;
                else d[(srcW - 1 - x) * srcH + y] = px;  // 270
            }
        }
    }
};

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;
VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;

static std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

bool VideoDecoder::open(const std::string& path, std::string& error) {
    close();
    auto p = std::make_unique<Impl>();
    int r = avformat_open_input(&p->fmt, path.c_str(), nullptr, nullptr);
    if (r < 0) { error = "cannot open " + path + ": " + avErr(r); return false; }
    r = avformat_find_stream_info(p->fmt, nullptr);
    if (r < 0) { error = "cannot read stream info: " + avErr(r); return false; }
    const AVCodec* dec = nullptr;
    r = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (r < 0 || !dec) { error = "no video stream: " + avErr(r < 0 ? r : AVERROR_DECODER_NOT_FOUND); return false; }
    p->stream = r;
    AVStream* st = p->fmt->streams[r];
    p->codec = avcodec_alloc_context3(dec);
    if (!p->codec) { error = "cannot allocate decoder"; return false; }
    r = avcodec_parameters_to_context(p->codec, st->codecpar);
    if (r < 0) { error = "cannot configure decoder: " + avErr(r); return false; }
    p->codec->thread_count = 0;
    r = avcodec_open2(p->codec, dec, nullptr);
    if (r < 0) { error = "cannot open decoder: " + avErr(r); return false; }

    AVRational fps = st->avg_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) fps = st->r_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) fps = {30, 1};
    av_reduce(&p->fps.num, &p->fps.den, fps.num, fps.den, INT32_MAX);
    p->timeBase = st->time_base;
    p->startPts = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;

    if (st->nb_frames > 0) {
        p->frameCount = (int)st->nb_frames;
    } else {
        double dur = 0;
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0) dur = st->duration * av_q2d(st->time_base);
        else if (p->fmt->duration != AV_NOPTS_VALUE) dur = (double)p->fmt->duration / AV_TIME_BASE;
        p->frameCount = (int)std::llround(dur * av_q2d(p->fps));
    }
    if (p->frameCount < 1) p->frameCount = 1;

    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (sd && sd->size >= 9 * sizeof(int32_t)) {
        double deg = -av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
        if (std::isfinite(deg)) {
            int q = (int)std::lround(deg / 90.0) % 4;
            if (q < 0) q += 4;
            p->rotation = q * 90;
        }
    }

    p->srcW = p->codec->width;
    p->srcH = p->codec->height;
    if (p->srcW <= 0 || p->srcH <= 0) { error = "invalid video dimensions"; return false; }
    p->sws = sws_getContext(p->srcW, p->srcH, p->codec->pix_fmt, p->srcW, p->srcH, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                            nullptr, nullptr, nullptr);
    if (!p->sws) { error = "cannot create pixel converter"; return false; }
    p->frame = av_frame_alloc();
    p->pkt = av_packet_alloc();
    if (!p->frame || !p->pkt) { error = "out of memory"; return false; }
    size_t bytes = (size_t)p->srcW * p->srcH * 4;
    p->conv.resize(bytes);
    if (p->rotation) p->out.resize(bytes);
    p_ = std::move(p);
    return true;
}

void VideoDecoder::close() { p_.reset(); }
bool VideoDecoder::isOpen() const { return p_ != nullptr; }
int VideoDecoder::width() const { return !p_ ? 0 : (p_->rotation % 180 ? p_->srcH : p_->srcW); }
int VideoDecoder::height() const { return !p_ ? 0 : (p_->rotation % 180 ? p_->srcW : p_->srcH); }
int VideoDecoder::fpsNum() const { return p_ ? p_->fps.num : 0; }
int VideoDecoder::fpsDen() const { return p_ ? p_->fps.den : 1; }
int VideoDecoder::frameCount() const { return p_ ? p_->frameCount : 0; }
int VideoDecoder::currentFrame() const { return p_ ? p_->current : -1; }

const uint8_t* VideoDecoder::rgba() const {
    if (!p_ || !p_->hasFrame) return nullptr;
    return p_->rotation ? p_->out.data() : p_->conv.data();
}

bool VideoDecoder::decodeFrame(int n) {
    if (!p_ || n < 0 || n >= p_->frameCount) return false;
    Impl& d = *p_;
    if (n == d.current && d.hasFrame) return true;

    // Short forward jumps (playback catching up after a slow UI frame) decode on from the
    // current position; a seek would rewind to the previous keyframe and cost far more.
    constexpr int kMaxForwardDecode = 64;
    const bool forward = d.current >= 0 && n > d.current && n <= d.current + kMaxForwardDecode;
    if (!forward) {
        int64_t ts = d.startPts + av_rescale_q(n, av_inv_q(d.fps), d.timeBase);
        if (av_seek_frame(d.fmt, d.stream, ts, AVSEEK_FLAG_BACKWARD) < 0) return false;
        avcodec_flush_buffers(d.codec);
        d.eof = false;
        d.hasFrame = false;  // the converted buffer may no longer match any frame before n
    }

    bool skipped = false;  // `last` holds the latest decoded frame before n
    AVFrame* last = av_frame_alloc();
    if (!last) return false;
    for (;;) {
        int idx = d.next();
        if (idx < 0) break;
        if (idx >= n) {
            av_frame_free(&last);
            d.convert();
            d.hasFrame = true;
            d.current = n;
            return true;
        }
        av_frame_unref(last);
        av_frame_move_ref(last, d.frame);
        skipped = true;
    }
    if (skipped) {  // EOF before n: show the last decoded frame
        av_frame_unref(d.frame);
        av_frame_move_ref(d.frame, last);
        d.convert();
        d.hasFrame = true;
    }
    av_frame_free(&last);
    if (!d.hasFrame) return false;
    d.current = n;
    return true;
}
