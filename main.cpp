#include <rtweekend.h>
#include <camera.h>
#include <hittable_list.h>
#include <image_io.h>
#include <scene.h>

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string scene_path, output_path;
    bool bad_args = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc)
            output_path = argv[++i];
        else if (scene_path.empty() && arg[0] != '-')
            scene_path = arg;
        else
            bad_args = true;
    }
    if (bad_args || scene_path.empty()) {
        std::cerr << "Usage: " << argv[0] << " <scene file> [-o out.png|out.ppm]\n"
                  << "       Without -o, a PPM image is written to stdout.\n";
        return 1;
    }

    hittable_list world, lights;
    camera cam;
    if (!load_scene(scene_path, world, lights, cam))
        return 1;

    auto pixels = cam.render(world, lights);
    return write_image(output_path, cam.image_width, cam.height(), pixels) ? 0 : 1;
}
