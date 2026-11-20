#pragma once
#include <cstdint>
#include <memory>
#include <string>

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(VideoDecoder&&) noexcept;
    VideoDecoder& operator=(VideoDecoder&&) noexcept;
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();
    bool isOpen() const;
    int width() const;  // after display rotation
    int height() const;
    int fpsNum() const;
    int fpsDen() const;
    int frameCount() const;
    int currentFrame() const;     // -1 before first decode
    bool decodeFrame(int n);      // false if n out of [0, frameCount) or decode error
    const uint8_t* rgba() const;  // width()*height()*4, tightly packed

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
