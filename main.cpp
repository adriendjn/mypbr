#include <rtweekend.h>
#include <bvh.h>
#include <image_io.h>
#include <scene.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

static int usage(const char* program) {
    std::cerr << "Usage: " << program << " <scene file> [-o out.png|.ppm|.hdr|.pfm]... [--save-every N] [--threads N]\n"
              << "       -o may be repeated to write several formats; without -o, a PPM\n"
              << "       image is written to stdout. --save-every N rewrites the outputs\n"
              << "       every N samples per pixel, so a long render can be stopped early.\n"
              << "       --threads N sets the worker threads (default: all hardware threads).\n";
    return 1;
}

int main(int argc, char* argv[]) {
    std::string scene_path;
    std::vector<std::string> outputs;
    int save_every = 0;
    int threads = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outputs.push_back(argv[++i]);
            if (!is_supported_image(outputs.back()))
                return usage(argv[0]);
        } else if (arg == "--save-every" && i + 1 < argc) {
            save_every = std::atoi(argv[++i]);
            if (save_every < 1)
                return usage(argv[0]);
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
            if (threads < 1)
                return usage(argv[0]);
        } else if (scene_path.empty() && arg[0] != '-') {
            scene_path = arg;
        } else {
            return usage(argv[0]);
        }
    }
    if (scene_path.empty() || (save_every > 0 && outputs.empty()))
        return usage(argv[0]);
    if (outputs.empty())
        outputs.push_back("");  // stdout

    scene sc;
    if (!load_scene(scene_path, sc))
        return 1;

    auto& cam = sc.cam;
    cam.threads = threads;
    bvh world(sc.world);
    auto save = [&](const std::vector<color>& pixels) {
        bool ok = true;
        for (const auto& path : outputs)
            ok &= write_image(path, cam.image_width, cam.height(), pixels, sc.display);
        return ok;
    };

    auto pixels = cam.render(world, sc.lights, save_every,
                             [&](int, const std::vector<color>& partial) { save(partial); });
    return save(pixels) ? 0 : 1;
}
