// Unit tests for include/ska/ska.h: binary round trip, validation, versioning, sampling, transforms.
#include <ska/ska.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool bitEq(float a, float b) { return std::memcmp(&a, &b, 4) == 0; }
bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

bool poseEq(const ska::Pose& a, const ska::Pose& b) {
    return bitEq(a.translation.x, b.translation.x) && bitEq(a.translation.y, b.translation.y) &&
           bitEq(a.rotation, b.rotation) && bitEq(a.scale.x, b.scale.x) && bitEq(a.scale.y, b.scale.y);
}

bool docEq(const ska::Document& a, const ska::Document& b) {
    if (a.name != b.name || !bitEq(a.pixelsPerUnit, b.pixelsPerUnit)) return false;
    if (a.bones.size() != b.bones.size() || a.clips.size() != b.clips.size()) return false;
    for (size_t i = 0; i < a.bones.size(); ++i) {
        const ska::Bone &x = a.bones[i], &y = b.bones[i];
        if (x.name != y.name || x.parent != y.parent || !bitEq(x.length, y.length) || !poseEq(x.rest, y.rest) ||
            x.color != y.color)
            return false;
    }
    for (size_t c = 0; c < a.clips.size(); ++c) {
        const ska::Clip &x = a.clips[c], &y = b.clips[c];
        if (x.name != y.name || x.fpsNum != y.fpsNum || x.fpsDen != y.fpsDen || x.frameCount != y.frameCount ||
            x.videoPath != y.videoPath || x.keys.size() != y.keys.size())
            return false;
        for (size_t k = 0; k < x.keys.size(); ++k) {
            const ska::Key &kx = x.keys[k], &ky = y.keys[k];
            if (kx.frame != ky.frame || kx.interp != ky.interp || kx.poses.size() != ky.poses.size()) return false;
            for (size_t p = 0; p < kx.poses.size(); ++p)
                if (!poseEq(kx.poses[p], ky.poses[p])) return false;
        }
    }
    return true;
}

ska::Pose pose(float tx, float ty, float rot, float sx = 1, float sy = 1) {
    ska::Pose p;
    p.translation = {tx, ty};
    p.rotation = rot;
    p.scale = {sx, sy};
    return p;
}

ska::Document makeDoc() {
    ska::Document d;
    d.name = "runner";
    d.pixelsPerUnit = 64.5f;
    const char* names[3] = {"spine", "thigh", "shin"};
    for (int i = 0; i < 3; ++i) {
        ska::Bone b;
        b.name = names[i];
        b.parent = i - 1;
        b.length = 40.25f + float(i) * 0.1f;
        b.rest = pose(10.1f * float(i), -3.3f, 0.123f * float(i + 1), 1.0f + 0.01f * float(i), 0.9f);
        b.color = 0xFF000000u | uint32_t(i * 0x112233);
        d.bones.push_back(b);
    }
    ska::Clip run;
    run.name = "run";
    run.fpsNum = 30000;
    run.fpsDen = 1001;
    run.frameCount = 48;
    run.videoPath = "vid/run.mp4";
    const int frames[3] = {0, 10, 20};
    const ska::Interp interps[3] = {ska::Interp::Linear, ska::Interp::Step, ska::Interp::Linear};
    for (int k = 0; k < 3; ++k) {
        ska::Key key;
        key.frame = frames[k];
        key.interp = interps[k];
        for (int b = 0; b < 3; ++b) key.poses.push_back(pose(0.7f * float(k + b), 1.9f, -0.31f * float(k), 1.5f, 0.75f));
        run.keys.push_back(key);
    }
    ska::Clip idle;
    idle.name = "idle";
    idle.frameCount = 60;
    d.clips.push_back(run);
    d.clips.push_back(idle);
    return d;
}

void put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = uint8_t(v);
    b[off + 1] = uint8_t(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + size_t(i)] = uint8_t(v >> (8 * i));
}
uint32_t get32(const std::vector<uint8_t>& b, size_t off) {
    return uint32_t(b[off]) | uint32_t(b[off + 1]) << 8 | uint32_t(b[off + 2]) << 16 | uint32_t(b[off + 3]) << 24;
}
void fixCrc(std::vector<uint8_t>& b) { put32(b, 16, ska::crc32(b.data() + 20, b.size() - 20)); }

size_t findChunk(const std::vector<uint8_t>& b, const char* id) {
    size_t off = 20;
    while (off + 8 <= b.size()) {
        if (std::memcmp(b.data() + off, id, 4) == 0) return off;
        off += 8 + get32(b, off + 4);
    }
    return 0;
}

void testFormat() {
    const ska::Document src = makeDoc();
    std::vector<uint8_t> bytes = ska::write(src);

    {  // round trip
        ska::Document out;
        std::string err;
        CHECK(ska::read(bytes.data(), bytes.size(), out, &err));
        CHECK(err.empty());
        CHECK(docEq(src, out));
    }
    {  // CRC flip at offset 30
        std::vector<uint8_t> b = bytes;
        b[30] ^= 0xFF;
        ska::Document out;
        std::string err;
        CHECK(!ska::read(b.data(), b.size(), out, &err));
        CHECK(err.find("CRC") != std::string::npos);
    }
    {  // forward compatibility: minor 7 + unknown chunk
        std::vector<uint8_t> b = bytes;
        put16(b, 6, 7);
        const uint8_t extra[12] = {'Z', 'Z', 'Z', 'Z', 4, 0, 0, 0, 1, 2, 3, 4};
        b.insert(b.end(), extra, extra + 12);
        put32(b, 12, get32(b, 12) + 1);
        fixCrc(b);
        ska::Document out;
        std::string err;
        CHECK(ska::read(b.data(), b.size(), out, &err));
        CHECK(docEq(src, out));
    }
    {  // major 2
        std::vector<uint8_t> b = bytes;
        put16(b, 4, 2);
        fixCrc(b);
        ska::Document out;
        std::string err;
        CHECK(!ska::read(b.data(), b.size(), out, &err));
        CHECK(err.find("major version") != std::string::npos);
    }
    {  // every truncated prefix fails
        bool allFail = true;
        for (size_t n = 0; n < bytes.size(); ++n) {
            std::vector<uint8_t> b(bytes.begin(), bytes.begin() + std::ptrdiff_t(n));
            ska::Document out;
            std::string err;
            if (ska::read(b.data(), b.size(), out, &err) || err.empty()) allFail = false;
        }
        CHECK(allFail);
    }
    {  // bone with parent == own index
        std::vector<uint8_t> b = bytes;
        const size_t bone = findChunk(b, "BONE");
        CHECK(bone != 0);
        const size_t rec1 = bone + 8 + 8 + 40;  // chunk hdr, count+recordSize, record 0
        put32(b, rec1 + 4, 1);
        fixCrc(b);
        ska::Document out;
        std::string err;
        CHECK(!ska::read(b.data(), b.size(), out, &err));
        CHECK(err.find("parent") != std::string::npos);
    }
    {  // bad magic
        std::vector<uint8_t> b = bytes;
        b[0] = 'X';
        ska::Document out;
        std::string err;
        CHECK(!ska::read(b.data(), b.size(), out, &err));
        CHECK(err == "not a .ska file");
    }
    {  // file round trip
        const std::string path = "/tmp/ska_tests_roundtrip.ska";
        std::string err;
        CHECK(ska::writeFile(path, src, &err));
        ska::Document out;
        CHECK(ska::readFile(path, out, &err));
        CHECK(docEq(src, out));
        std::remove(path.c_str());
    }
}

ska::Document oneBone() {
    ska::Document d;
    d.bones.push_back(ska::Bone{});
    return d;
}

ska::Key key(int frame, float rot, ska::Interp interp = ska::Interp::Linear) {
    ska::Key k;
    k.frame = frame;
    k.interp = interp;
    k.poses.push_back(pose(float(frame), 0, rot));
    return k;
}

void testSampling() {
    const ska::Document d = oneBone();
    std::vector<ska::Pose> out;
    {  // shortest-path rotation
        ska::Clip c;
        c.keys = {key(0, 170 * kPi / 180), key(10, -170 * kPi / 180)};
        ska::sampleFrame(d, c, 5, out);
        CHECK(out.size() == 1);
        CHECK(near(std::fabs(out[0].rotation), kPi, 1e-4f));
        CHECK(near(out[0].translation.x, 5, 1e-5f));
    }
    {  // step
        ska::Clip c;
        c.keys = {key(0, 0.5f, ska::Interp::Step), key(10, 1.5f)};
        ska::sampleFrame(d, c, 5, out);
        CHECK(poseEq(out[0], c.keys[0].poses[0]));
    }
    {  // clamping before first / after last
        ska::Clip c;
        c.keys = {key(5, 0.25f), key(10, 0.75f)};
        ska::sampleFrame(d, c, 2, out);
        CHECK(poseEq(out[0], c.keys[0].poses[0]));
        ska::sampleFrame(d, c, 50, out);
        CHECK(poseEq(out[0], c.keys[1].poses[0]));
    }
    {  // no keys -> rest
        ska::Document r = oneBone();
        r.bones[0].rest = pose(3, 4, 0.5f);
        ska::Clip c;
        ska::sampleFrame(r, c, 3, out);
        CHECK(poseEq(out[0], r.bones[0].rest));
    }
    {  // sampleTime loop wrap: 20 frames @ 10 fps, t = 2.1 s -> frame 1
        ska::Clip c;
        c.fpsNum = 10;
        c.fpsDen = 1;
        c.frameCount = 20;
        c.keys = {key(0, 0), key(10, 1)};
        ska::sampleTime(d, c, 2.1f, true, out);
        CHECK(near(out[0].rotation, 0.1f, 1e-4f));
        ska::sampleTime(d, c, 2.1f, false, out);
        CHECK(near(out[0].rotation, 1.0f, 1e-6f));
    }
}

void testWorld() {
    ska::Document d;
    ska::Bone root, child;
    root.rest = pose(100, 100, kPi / 2);
    child.parent = 0;
    child.rest = pose(50, 0, 0);
    d.bones = {root, child};
    std::vector<ska::Pose> local;
    std::vector<ska::Affine> world;
    ska::restPose(d, local);
    ska::computeWorld(d, local, world);
    CHECK(world.size() == 2);
    CHECK(near(world[1].tx, 100, 1e-4f));
    CHECK(near(world[1].ty, 150, 1e-4f));
    const ska::Affine inv = ska::inverse(world[1]);
    const ska::Vec2 back = ska::apply(inv, {world[1].tx, world[1].ty});
    CHECK(near(back.x, 0, 1e-4f) && near(back.y, 0, 1e-4f));
    CHECK(near(ska::wrapPi(3 * kPi), kPi, 1e-5f));
    const uint8_t abc[3] = {'a', 'b', 'c'};
    CHECK(ska::crc32(abc, 3) == 0x352441C2u);
}

}  // namespace

int main() {
    testFormat();
    testSampling();
    testWorld();
    std::printf("ska_tests: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
