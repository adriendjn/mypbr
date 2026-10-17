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
              << "       [--denoise [--noisy noisy.png]...]\n"
              << "       -o may be repeated to write several formats; without -o, a PPM\n"
              << "       image is written to stdout. --save-every N rewrites the outputs\n"
              << "       every N samples per pixel, so a long render can be stopped early.\n"
              << "       --threads N sets the worker threads (default: all hardware threads).\n"
              << "       --denoise filters the -o outputs with an albedo/normal guided\n"
              << "       wavelet denoiser; --noisy also writes the unfiltered render.\n";
    return 1;
}

int main(int argc, char* argv[]) {
    std::string scene_path;
    std::vector<std::string> outputs;
    std::vector<std::string> noisy_outputs;
    bool denoise = false;
    int save_every = 0;
    int threads = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outputs.push_back(argv[++i]);
            if (!is_supported_image(outputs.back()))
                return usage(argv[0]);
        } else if (arg == "--noisy" && i + 1 < argc) {
            noisy_outputs.push_back(argv[++i]);
            if (!is_supported_image(noisy_outputs.back()))
                return usage(argv[0]);
        } else if (arg == "--denoise") {
            denoise = true;
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
    if (scene_path.empty() || (save_every > 0 && outputs.empty()) || (!noisy_outputs.empty() && !denoise))
        return usage(argv[0]);
    if (outputs.empty())
        outputs.push_back("");  // stdout

    scene sc;
    if (!load_scene(scene_path, sc))
        return 1;

    auto& cam = sc.cam;
    cam.threads = threads;
    cam.collect_features = denoise;
    // The BVH, plus the objects it can't hold (an infinite ground) beside it
    auto scene_bvh = std::make_shared<bvh>(sc.world);
    hittable_list everything(scene_bvh);
    for (const auto& object : sc.unbounded.objects)
        everything.add(object);
    const hittable& world = sc.unbounded.empty() ? static_cast<const hittable&>(*scene_bvh) : everything;

    denoiser filter;
    filter.threads = threads;
    auto save = [&](const frame& f) {
        bool ok = true;
        auto write_all = [&](const std::vector<std::string>& paths, const std::vector<color>& pixels) {
            for (const auto& path : paths)
                ok &= write_image(path, cam.image_width, cam.height(), pixels, sc.display);
        };
        if (denoise) {
            write_all(outputs, filter.run(f, cam.image_width, cam.height()));
            write_all(noisy_outputs, f.radiance);
        } else {
            write_all(outputs, f.radiance);
        }
        return ok;
    };

    auto result = cam.render(world, sc.lights, save_every, [&](int, const frame& partial) { save(partial); });
    return save(result) ? 0 : 1;
}
