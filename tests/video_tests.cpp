#include "video/video_decoder.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static std::vector<uint8_t> copyFrame(const VideoDecoder& v) {
    const uint8_t* p = v.rgba();
    if (!p) return {};
    return std::vector<uint8_t>(p, p + (size_t)v.width() * v.height() * 4);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: video_tests <test.mp4> <test_rot.mp4>\n");
        return 2;
    }
    std::string err;
    {
        VideoDecoder v;
        bool ok = v.open(argv[1], err);
        CHECK(ok);
        if (!ok) std::printf("open error: %s\n", err.c_str());
        CHECK(v.width() == 640 && v.height() == 360);
        CHECK(v.fpsNum() == 30 && v.fpsDen() == 1);
        CHECK(v.frameCount() == 150);
        CHECK(v.currentFrame() == -1);

        bool seqOk = true;
        for (int i = 0; i <= 100; ++i) seqOk = seqOk && v.decodeFrame(i);
        CHECK(seqOk);
        CHECK(v.currentFrame() == 100);
        std::vector<uint8_t> seq = copyFrame(v);

        // Forward jump of a few frames (playback catch-up) matches a seek-decoded frame.
        CHECK(v.decodeFrame(103));
        CHECK(v.currentFrame() == 103);
        std::vector<uint8_t> jumped = copyFrame(v);
        VideoDecoder x;
        CHECK(x.open(argv[1], err));
        CHECK(x.decodeFrame(103));
        CHECK(!jumped.empty() && jumped == copyFrame(x));

        VideoDecoder w;
        CHECK(w.open(argv[1], err));
        CHECK(w.decodeFrame(0));
        std::vector<uint8_t> f0 = copyFrame(w);
        CHECK(w.decodeFrame(140));
        CHECK(w.decodeFrame(100));
        std::vector<uint8_t> sought = copyFrame(w);
        CHECK(!seq.empty() && seq == sought);
        CHECK(seq != f0);

        CHECK(w.decodeFrame(99));
        CHECK(w.currentFrame() == 99);
        CHECK(copyFrame(w) != sought);
        CHECK(!w.decodeFrame(150));
        CHECK(!w.decodeFrame(-1));
        CHECK(w.decodeFrame(149));

        VideoDecoder moved(std::move(w));
        CHECK(moved.isOpen() && !w.isOpen());
        CHECK(moved.currentFrame() == 149);
        moved.close();
        CHECK(!moved.isOpen());
    }
    {
        VideoDecoder r;
        CHECK(r.open(argv[2], err));
        CHECK(r.width() == 360 && r.height() == 640);
        CHECK(r.decodeFrame(10));
        CHECK(r.rgba() != nullptr);
    }
    {
        VideoDecoder bad;
        CHECK(!bad.open("/nonexistent/file.mp4", err));
        CHECK(!err.empty());
    }
    std::printf("video_tests: %s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
