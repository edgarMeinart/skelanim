// ska_dump: print the contents of a .ska file.
#include <ska/ska.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: ska_dump <file.ska>\n");
        return 1;
    }
    ska::Document doc;
    std::string error;
    if (!ska::readFile(argv[1], doc, &error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    // The version lives only in the file header (offsets 4/6); readFile already validated it.
    unsigned major = ska::kVersionMajor, minor = ska::kVersionMinor;
    if (std::FILE* f = std::fopen(argv[1], "rb")) {
        uint8_t h[8];
        if (std::fread(h, 1, sizeof h, f) == sizeof h) {
            major = unsigned(h[4] | (h[5] << 8));
            minor = unsigned(h[6] | (h[7] << 8));
        }
        std::fclose(f);
    }
    std::printf("version %u.%u ppu %g name \"%s\"\n", major, minor, double(doc.pixelsPerUnit), doc.name.c_str());
    std::printf("bones %zu\n", doc.bones.size());
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        const ska::Bone& b = doc.bones[i];
        std::printf("  [%zu] \"%s\" parent %d len %g rest t=(%g,%g) r=%g\u00b0 s=(%g,%g)\n", i, b.name.c_str(),
                    int(b.parent), double(b.length), double(b.rest.translation.x), double(b.rest.translation.y),
                    double(b.rest.rotation) * 180.0 / 3.14159265358979323846, double(b.rest.scale.x),
                    double(b.rest.scale.y));
    }
    std::printf("clips %zu\n", doc.clips.size());
    for (const ska::Clip& c : doc.clips) {
        std::printf("  \"%s\" fps %u/%u frames %u video \"%s\" keys %zu\n", c.name.c_str(), c.fpsNum, c.fpsDen,
                    c.frameCount, c.videoPath.c_str(), c.keys.size());
        std::printf("    keys:");
        for (const ska::Key& k : c.keys) std::printf(" %d", int(k.frame));
        std::printf("\n");
    }
    return 0;
}
