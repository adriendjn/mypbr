// The voxel grid (3D DDA) must find exactly the surfaces that the same
// model's faces do as quads in a BVH: same distance, normal, side and
// material, for rays from outside the grid, from inside it, through glass,
// and clipped by ray_t.

#include <rtweekend.h>
#include <voxel.h>
#include <vox_loader.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

int main() {
    // A random model mixing empty, opaque, glass (transparent) and emissive
    // cells, with a solid glass block in one corner
    vox_file vox;
    for (int k = 0; k < 256; k++)
        vox.palette[k] = color((k % 7) / 7.0, (k % 5) / 5.0, (k % 3) / 3.0);
    vox.materials[3]["_type"] = "_glass";
    vox.materials[4]["_type"] = "_glass";
    vox.materials[5]["_type"] = "_emit";
    vox_model m;
    m.nx = 13; m.ny = 9; m.nz = 11;
    m.cells.assign(size_t(m.nx) * m.ny * m.nz, 0);
    for (auto& c : m.cells) {
        double x = random_double();
        c = x < 0.55 ? 0 : x < 0.8 ? 1 + int(random_double() * 2) : x < 0.95 ? 3 + int(random_double() * 2) : 5;
    }
    for (int z = 0; z < 4; z++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                m.cells[(size_t(z) * m.ny + y) * m.nx + x] = 3;

    auto palette = std::make_shared<voxel_palette>(make_voxel_palette(vox, 1));
    hittable_list faces, emitters;
    voxel_model_faces(m, *palette, faces, emitters);
    bvh reference(faces);
    auto model = std::make_shared<vox_model>(m);

    bool ok = true;
    // One block holding the whole model, and blocks of 4 and 5 cells, whose
    // boundaries cross the glass and the other surfaces
    for (int block : {64, 4, 5}) {
        auto grid_ptr = voxel_blocks(model, palette, block);
        const hittable& grid = *grid_ptr;
        int mismatches = 0, hits = 0;
        const int rays = 200000;
        for (int k = 0; k < rays; k++) {
            // Origins outside the box aimed at a point inside, or inside it in
            // any direction; some with a finite ray_t.max
            point3 target(random_double(0, m.nx), random_double(0, m.ny), random_double(0, m.nz));
            point3 origin = k % 2 ? target + 30 * random_unit_vector() : target;
            vec3 direction = k % 2 ? target - origin : random_unit_vector() * random_double(0.5, 2);
            interval ray_t(0.001, k % 3 == 0 ? random_double(0.1, 40) : infinity);
            ray r(origin, direction);

            hit_record a, b;
            bool hit_a = reference.hit(r, ray_t, a);
            bool hit_b = grid.hit(r, ray_t, b);
            bool same = hit_a == hit_b;
            if (same && hit_a) {
                hits++;
                same = std::fabs(a.t - b.t) <= 1e-9 * std::fmax(1.0, a.t) && (a.normal - b.normal).near_zero()
                    && a.front_face == b.front_face && a.mat == b.mat;
            }
            if (!same && ++mismatches <= 5) {
                std::cerr << "Mismatch for ray " << origin << " -> " << direction << ": faces "
                          << (hit_a ? "t " + std::to_string(a.t) : std::string("miss")) << ", grid "
                          << (hit_b ? "t " + std::to_string(b.t) : std::string("miss")) << '\n';
            }
        }

        // Rays exactly along a grid plane or axis-aligned through cells hit
        // edges where two faces meet, which either may report; the random rays
        // above avoid those, so a few mismatches would mean a real difference
        std::cout << (mismatches == 0 ? "PASS" : "FAIL") << " voxel grid in blocks of " << block
                  << " matches its faces: " << mismatches << " mismatches in " << rays << " rays ("
                  << hits << " hits)\n";
        ok &= mismatches == 0;
    }
    return ok ? 0 : 1;
}
