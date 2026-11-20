// ska.h - SkelAnim 2D skeletal animation: data model, binary format (.ska), sampling.
// Header-only, C++17. See docs/ska-format.md for the format specification.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace ska {

constexpr uint16_t kVersionMajor = 1, kVersionMinor = 0;
constexpr uint32_t kNoString = 0xFFFFFFFFu;

struct Vec2 {
    float x = 0, y = 0;
};

struct Pose {
    Vec2 translation;
    float rotation = 0;  // radians, positive = clockwise on screen (+y down)
    Vec2 scale{1, 1};
};

struct Bone {
    std::string name;
    int32_t parent = -1;  // -1 = root, otherwise < own index
    float length = 50;
    Pose rest;
    uint32_t color = 0xFF3399FFu;  // IM_COL32 layout: R in bits 0-7, A in bits 24-31
};

enum class Interp : uint8_t { Linear = 0, Step = 1 };

struct Key {
    int32_t frame = 0;
    Interp interp = Interp::Linear;
    std::vector<Pose> poses;  // size == Document::bones.size()
};

struct Clip {
    std::string name;
    uint32_t fpsNum = 30, fpsDen = 1, frameCount = 120;
    std::string videoPath;  // editor-only reference, games ignore it
    std::vector<Key> keys;  // strictly ascending frame
};

struct Document {
    std::string name;
    float pixelsPerUnit = 100;
    std::vector<Bone> bones;  // parent < own index
    std::vector<Clip> clips;
};

// x' = a*x + c*y + tx ; y' = b*x + d*y + ty
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};

// ---------------------------------------------------------------------------
// Math

inline Affine toAffine(const Pose& p) {
    const float cs = std::cos(p.rotation), sn = std::sin(p.rotation);
    Affine m;
    m.a = cs * p.scale.x;
    m.b = sn * p.scale.x;
    m.c = -sn * p.scale.y;
    m.d = cs * p.scale.y;
    m.tx = p.translation.x;
    m.ty = p.translation.y;
    return m;
}

inline Affine mul(const Affine& p, const Affine& c) {
    Affine m;
    m.a = p.a * c.a + p.c * c.b;
    m.b = p.b * c.a + p.d * c.b;
    m.c = p.a * c.c + p.c * c.d;
    m.d = p.b * c.c + p.d * c.d;
    m.tx = p.a * c.tx + p.c * c.ty + p.tx;
    m.ty = p.b * c.tx + p.d * c.ty + p.ty;
    return m;
}

inline Affine inverse(const Affine& m) {
    const float det = m.a * m.d - m.b * m.c;
    const float inv = det != 0 ? 1.0f / det : 0.0f;
    Affine r;
    r.a = m.d * inv;
    r.b = -m.b * inv;
    r.c = -m.c * inv;
    r.d = m.a * inv;
    r.tx = -(r.a * m.tx + r.c * m.ty);
    r.ty = -(r.b * m.tx + r.d * m.ty);
    return r;
}

inline Vec2 apply(const Affine& m, Vec2 v) {
    return {m.a * v.x + m.c * v.y + m.tx, m.b * v.x + m.d * v.y + m.ty};
}

// Assumes no shear: sx = |(a,b)|, rotation = atan2(b,a), sy = det / sx.
inline Pose decompose(const Affine& m) {
    Pose p;
    p.translation = {m.tx, m.ty};
    const float sx = std::sqrt(m.a * m.a + m.b * m.b);
    p.scale.x = sx;
    p.rotation = std::atan2(m.b, m.a);
    p.scale.y = sx != 0 ? (m.a * m.d - m.b * m.c) / sx : 0.0f;
    return p;
}

inline float wrapPi(float a) {
    constexpr float kPi = 3.14159265358979323846f, kTwoPi = 2 * kPi;
    a = std::fmod(a + kPi, kTwoPi);
    if (a <= 0) a += kTwoPi;
    return a - kPi;
}

inline uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Binary format

namespace detail {

constexpr size_t kHeaderSize = 20;
constexpr uint32_t kMetaMin = 8, kBoneRecord = 40, kClipHeader = 36, kKeyRecord = 8, kPoseRecord = 20;

struct Writer {
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) {
        u8(uint8_t(v));
        u8(uint8_t(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(uint8_t(v >> (8 * i)));
    }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f32(float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    void put32At(size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) buf[off + i] = uint8_t(v >> (8 * i));
    }
    void pose(const Pose& p) {
        f32(p.translation.x);
        f32(p.translation.y);
        f32(p.rotation);
        f32(p.scale.x);
        f32(p.scale.y);
    }
};

struct Reader {
    const uint8_t* p;
    size_t size;
    size_t pos = 0;
    bool ok = true;
    Reader(const uint8_t* data, size_t n) : p(data), size(n) {}
    bool need(size_t n) {
        if (!ok || size - pos < n) {
            ok = false;
            return false;
        }
        return true;
    }
    uint8_t u8() {
        if (!need(1)) return 0;
        return p[pos++];
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = uint16_t(p[pos] | (p[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = uint32_t(p[pos]) | (uint32_t(p[pos + 1]) << 8) | (uint32_t(p[pos + 2]) << 16) |
                     (uint32_t(p[pos + 3]) << 24);
        pos += 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    float f32() {
        uint32_t u = u32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    void skip(size_t n) {
        if (need(n)) pos += n;
    }
    Reader sub(size_t n) {
        if (!need(n)) return Reader(p, 0);
        Reader r(p + pos, n);
        pos += n;
        return r;
    }
};

struct StringTable {
    std::vector<std::string> list;
    std::unordered_map<std::string, uint32_t> index;
    uint32_t add(const std::string& s) {
        if (s.empty()) return kNoString;
        auto it = index.find(s);
        if (it != index.end()) return it->second;
        uint32_t id = uint32_t(list.size());
        list.push_back(s);
        index.emplace(s, id);
        return id;
    }
};

inline bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

inline bool finite(const Pose& p) {
    return std::isfinite(p.translation.x) && std::isfinite(p.translation.y) && std::isfinite(p.rotation) &&
           std::isfinite(p.scale.x) && std::isfinite(p.scale.y);
}

inline Pose readPose(Reader& r) {
    Pose p;
    p.translation.x = r.f32();
    p.translation.y = r.f32();
    p.rotation = r.f32();
    p.scale.x = r.f32();
    p.scale.y = r.f32();
    return p;
}

}  // namespace detail

inline std::vector<uint8_t> write(const Document& doc) {
    using namespace detail;
    StringTable strs;
    const uint32_t docName = strs.add(doc.name);
    std::vector<uint32_t> boneNames, clipNames, clipVideos;
    for (const Bone& b : doc.bones) boneNames.push_back(strs.add(b.name));
    for (const Clip& c : doc.clips) {
        clipNames.push_back(strs.add(c.name));
        clipVideos.push_back(strs.add(c.videoPath));
    }

    Writer w;
    w.bytes("SKA1", 4);
    w.u16(kVersionMajor);
    w.u16(kVersionMinor);
    w.u32(0);  // flags
    w.u32(uint32_t(3 + doc.clips.size()));
    w.u32(0);  // crc, patched below

    auto beginChunk = [&](const char* id) {
        w.bytes(id, 4);
        w.u32(0);
        return w.buf.size();
    };
    auto endChunk = [&](size_t start) { w.put32At(start - 4, uint32_t(w.buf.size() - start)); };

    size_t c = beginChunk("META");
    w.f32(doc.pixelsPerUnit);
    w.u32(docName);
    endChunk(c);

    c = beginChunk("STRS");
    w.u32(uint32_t(strs.list.size()));
    for (const std::string& s : strs.list) {
        w.u32(uint32_t(s.size()));
        w.bytes(s.data(), s.size());
    }
    endChunk(c);

    c = beginChunk("BONE");
    w.u32(uint32_t(doc.bones.size()));
    w.u32(kBoneRecord);
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        const Bone& b = doc.bones[i];
        w.u32(boneNames[i]);
        w.i32(b.parent);
        w.f32(b.length);
        w.pose(b.rest);
        w.u32(b.color);
        w.u32(0);
    }
    endChunk(c);

    for (size_t ci = 0; ci < doc.clips.size(); ++ci) {
        const Clip& clip = doc.clips[ci];
        c = beginChunk("CLIP");
        w.u32(kClipHeader);
        w.u32(clipNames[ci]);
        w.u32(clip.fpsNum);
        w.u32(clip.fpsDen);
        w.u32(clip.frameCount);
        w.u32(clipVideos[ci]);
        w.u32(uint32_t(doc.bones.size()));
        w.u32(uint32_t(clip.keys.size()));
        w.u32(kKeyRecord);
        w.u32(kPoseRecord);
        for (const Key& k : clip.keys) {
            w.i32(k.frame);
            w.u8(uint8_t(k.interp));
            w.u8(0);
            w.u8(0);
            w.u8(0);
            for (size_t b = 0; b < doc.bones.size(); ++b) w.pose(b < k.poses.size() ? k.poses[b] : doc.bones[b].rest);
        }
        endChunk(c);
    }

    w.put32At(16, crc32(w.buf.data() + kHeaderSize, w.buf.size() - kHeaderSize));
    return std::move(w.buf);
}

inline bool writeFile(const std::string& path, const Document& doc, std::string* error) {
    const std::vector<uint8_t> bytes = write(doc);
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return detail::fail(error, "cannot open " + tmp + " for writing");
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        std::remove(tmp.c_str());
        return detail::fail(error, "write failed: " + tmp);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::remove(tmp.c_str());
        return detail::fail(error, "cannot replace " + path + ": " + ec.message());
    }
    return true;
}

inline bool read(const uint8_t* data, size_t size, Document& out, std::string* error) {
    using namespace detail;
    using detail::fail;
    if (size < 4 || std::memcmp(data, "SKA1", 4) != 0) return fail(error, "not a .ska file");
    if (size < kHeaderSize) return fail(error, "truncated file");
    Reader h(data, size);
    h.skip(4);
    const uint16_t major = h.u16();
    h.u16();  // minor: any accepted
    h.u32();  // flags: ignored
    const uint32_t chunkCount = h.u32();
    const uint32_t crc = h.u32();
    if (major != kVersionMajor) return fail(error, "unsupported major version " + std::to_string(major));
    if (crc32(data + kHeaderSize, size - kHeaderSize) != crc) return fail(error, "CRC mismatch");

    struct Span {
        char id[4];
        Reader r;
    };
    std::vector<Span> chunks;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        if (!h.need(8)) return fail(error, "truncated file");
        Span s{{}, Reader(data, 0)};
        std::memcpy(s.id, data + h.pos, 4);
        h.skip(4);
        const uint32_t len = h.u32();
        s.r = h.sub(len);
        if (!h.ok) return fail(error, "truncated file");
        chunks.push_back(s);
    }

    auto isId = [](const Span& s, const char* id) { return std::memcmp(s.id, id, 4) == 0; };
    const Span *meta = nullptr, *strsChunk = nullptr, *bone = nullptr;
    std::vector<const Span*> clips;
    for (const Span& s : chunks) {
        const Span** slot = isId(s, "META") ? &meta : isId(s, "STRS") ? &strsChunk : isId(s, "BONE") ? &bone : nullptr;
        if (slot) {
            if (*slot) return fail(error, "duplicate " + std::string(s.id, 4) + " chunk");
            *slot = &s;
        } else if (isId(s, "CLIP")) {
            clips.push_back(&s);
        }
    }
    if (!meta) return fail(error, "missing META chunk");
    if (!strsChunk) return fail(error, "missing STRS chunk");
    if (!bone) return fail(error, "missing BONE chunk");

    Document doc;

    // STRS
    std::vector<std::string> strs;
    {
        Reader r = strsChunk->r;
        const uint32_t count = r.u32();
        if (!r.ok) return fail(error, "truncated file");
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t len = r.u32();
            if (!r.need(len)) return fail(error, "truncated file");
            strs.emplace_back(reinterpret_cast<const char*>(r.p + r.pos), len);
            r.skip(len);
        }
    }
    bool badString = false;
    auto str = [&](uint32_t idx) -> std::string {
        if (idx == kNoString) return {};
        if (idx >= strs.size()) {
            badString = true;
            return {};
        }
        return strs[idx];
    };

    // META
    {
        Reader r = meta->r;
        if (r.size < kMetaMin) return fail(error, "META chunk too small");
        doc.pixelsPerUnit = r.f32();
        doc.name = str(r.u32());
        if (!std::isfinite(doc.pixelsPerUnit)) return fail(error, "non-finite value in META");
        if (badString) return fail(error, "string index out of range");
    }

    // BONE
    {
        Reader r = bone->r;
        const uint32_t count = r.u32();
        const uint32_t rec = r.u32();
        if (!r.ok) return fail(error, "truncated file");
        if (rec < kBoneRecord) return fail(error, "bone record size too small");
        if (uint64_t(count) * rec > r.size - r.pos) return fail(error, "truncated file");
        doc.bones.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            Reader br = r.sub(rec);
            Bone& b = doc.bones[i];
            b.name = str(br.u32());
            b.parent = br.i32();
            b.length = br.f32();
            b.rest = readPose(br);
            b.color = br.u32();
            br.u32();  // flags
            if (badString) return fail(error, "string index out of range");
            if (b.parent < -1 || b.parent >= int32_t(i))
                return fail(error, "bone " + std::to_string(i) + " has invalid parent " + std::to_string(b.parent));
            if (!std::isfinite(b.length) || !finite(b.rest))
                return fail(error, "non-finite value in bone " + std::to_string(i));
        }
    }

    // CLIPs
    for (const Span* s : clips) {
        Reader r = s->r;
        const uint32_t hdrSize = r.u32();
        if (!r.ok) return fail(error, "truncated file");
        if (hdrSize < kClipHeader) return fail(error, "clip header size too small");
        Reader hr = r.sub(hdrSize);
        if (!r.ok) return fail(error, "truncated file");
        Clip clip;
        clip.name = str(hr.u32());
        clip.fpsNum = hr.u32();
        clip.fpsDen = hr.u32();
        clip.frameCount = hr.u32();
        clip.videoPath = str(hr.u32());
        const uint32_t boneCount = hr.u32();
        const uint32_t keyCount = hr.u32();
        const uint32_t keyRec = hr.u32();
        const uint32_t poseRec = hr.u32();
        if (badString) return fail(error, "string index out of range");
        if (clip.fpsNum == 0 || clip.fpsDen == 0) return fail(error, "clip has zero fps");
        if (clip.frameCount == 0) return fail(error, "clip has zero frames");
        if (boneCount != doc.bones.size()) return fail(error, "clip bone count does not match skeleton");
        if (keyRec < kKeyRecord) return fail(error, "key record size too small");
        if (poseRec < kPoseRecord) return fail(error, "pose record size too small");
        const uint64_t perKey = keyRec + uint64_t(boneCount) * poseRec;
        if (uint64_t(keyCount) * perKey > r.size - r.pos) return fail(error, "truncated file");
        clip.keys.resize(keyCount);
        for (uint32_t k = 0; k < keyCount; ++k) {
            Key& key = clip.keys[k];
            Reader kr = r.sub(keyRec);
            key.frame = kr.i32();
            const uint8_t interp = kr.u8();
            if (interp > 1) return fail(error, "invalid interpolation mode " + std::to_string(interp));
            key.interp = Interp(interp);
            if (key.frame < 0 || uint32_t(key.frame) >= clip.frameCount)
                return fail(error, "key frame " + std::to_string(key.frame) + " out of range");
            if (k > 0 && key.frame <= clip.keys[k - 1].frame) return fail(error, "key frames not strictly ascending");
            key.poses.resize(boneCount);
            for (uint32_t b = 0; b < boneCount; ++b) {
                Reader pr = r.sub(poseRec);
                key.poses[b] = readPose(pr);
                if (!finite(key.poses[b])) return fail(error, "non-finite value in key pose");
            }
        }
        if (!r.ok) return fail(error, "truncated file");
        doc.clips.push_back(std::move(clip));
    }

    out = std::move(doc);
    return true;
}

inline bool readFile(const std::string& path, Document& out, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return detail::fail(error, "cannot open " + path);
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) return detail::fail(error, "read error: " + path);
    return read(bytes.data(), bytes.size(), out, error);
}

// ---------------------------------------------------------------------------
// Sampling

inline void restPose(const Document& doc, std::vector<Pose>& out) {
    out.resize(doc.bones.size());
    for (size_t i = 0; i < doc.bones.size(); ++i) out[i] = doc.bones[i].rest;
}

inline void sampleFrame(const Document& doc, const Clip& clip, float frame, std::vector<Pose>& out) {
    if (clip.keys.empty()) {
        restPose(doc, out);
        return;
    }
    const size_t n = doc.bones.size();
    out.resize(n);
    auto copy = [&](const Key& k) {
        for (size_t i = 0; i < n; ++i) out[i] = i < k.poses.size() ? k.poses[i] : doc.bones[i].rest;
    };
    if (frame <= float(clip.keys.front().frame)) return copy(clip.keys.front());
    if (frame >= float(clip.keys.back().frame)) return copy(clip.keys.back());
    auto it = std::upper_bound(clip.keys.begin(), clip.keys.end(), frame,
                               [](float f, const Key& k) { return f < float(k.frame); });
    const Key& k1 = *it;
    const Key& k0 = *(it - 1);
    if (k0.interp == Interp::Step) return copy(k0);
    const float t = (frame - float(k0.frame)) / float(k1.frame - k0.frame);
    for (size_t i = 0; i < n; ++i) {
        const Pose a = i < k0.poses.size() ? k0.poses[i] : doc.bones[i].rest;
        const Pose b = i < k1.poses.size() ? k1.poses[i] : doc.bones[i].rest;
        Pose& p = out[i];
        p.translation.x = a.translation.x + (b.translation.x - a.translation.x) * t;
        p.translation.y = a.translation.y + (b.translation.y - a.translation.y) * t;
        p.rotation = a.rotation + wrapPi(b.rotation - a.rotation) * t;
        p.scale.x = a.scale.x + (b.scale.x - a.scale.x) * t;
        p.scale.y = a.scale.y + (b.scale.y - a.scale.y) * t;
    }
}

inline void sampleTime(const Document& doc, const Clip& clip, float seconds, bool loop, std::vector<Pose>& out) {
    float frame = seconds * float(clip.fpsNum) / float(clip.fpsDen ? clip.fpsDen : 1);
    if (loop && clip.frameCount > 0) {
        frame = std::fmod(frame, float(clip.frameCount));
        if (frame < 0) frame += float(clip.frameCount);
    }
    sampleFrame(doc, clip, frame, out);
}

inline void computeWorld(const Document& doc, const std::vector<Pose>& local, std::vector<Affine>& world) {
    world.resize(local.size());
    for (size_t i = 0; i < local.size(); ++i) {
        const int p = i < doc.bones.size() ? doc.bones[i].parent : -1;
        const Affine l = toAffine(local[i]);
        world[i] = p < 0 ? l : mul(world[size_t(p)], l);
    }
}

}  // namespace ska
