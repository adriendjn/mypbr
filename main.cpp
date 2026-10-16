#include <rtweekend.h>
#include <camera.h>
#include <hittable_list.h>
#include <scene.h>

#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <scene file> > image.ppm\n";
        return 1;
    }

    hittable_list world;
    camera cam;
    if (!load_scene(argv[1], world, cam))
        return 1;

    cam.render(world);
}
