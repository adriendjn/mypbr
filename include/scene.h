#ifndef SCENE_H
#define SCENE_H

#include "atmosphere.h"
#include "camera.h"
#include "hittable_list.h"
#include "instance.h"
#include "light_list.h"
#include "material.h"
#include "medium.h"
#include "obj_loader.h"
#include "plane.h"
#include "quad.h"
#include "sphere.h"
#include "sun.h"
#include "texture.h"
#include "tonemap.h"
#include "transform.h"
#include "voxel.h"

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>

// Everything a scene file describes.
class scene{
    public:
        hittable_list    world;
        hittable_list    unbounded;  // Objects without a finite bounding box (the
                                     // ground), tested beside world's BVH
        light_list       lights;   // Emissive objects (also part of world) and the environment
        camera           cam;
        display_settings display;  // How radiance becomes PNG/PPM pixels
};

// Loads a scene description.
//
// The file is read line by line. Each line starts with a keyword followed by
// its values; blank lines and anything after '#' are ignored:
//
//   image_width  800
//   aspect_ratio 16/9          # or a plain number, e.g. 1.5
//   samples_per_pixel 100      # random samples per pixel (antialiasing)
//   max_depth    50            # hard cap on bounces (Russian roulette ends most paths earlier)
//
//   camera lookfrom 13 2 3  lookat 0 0 0  vup 0 1 0  vfov 20
//   aperture     0.6           # defocus angle in degrees (0 = pinhole)
//   camera blades 6 blade_rotation 15   # polygonal aperture: hexagonal bokeh
//   camera ortho 40            # orthographic projection showing 40 units of height
//                              # (0 = perspective); from lookfrom along (1, 1, 1)
//                              # towards lookat, it is the isometric view
//
//   bounces diffuse 2 specular 5 scatter 5   # MagicaVoxel-style caps on each kind
//                              # of bounce (0 = no cap): fewer diffuse bounces render
//                              # faster and darken crevices (a look, not physics)
//   transparent_shadows on     # shadow rays cross smooth glass in a straight line,
//                              # tinted (MagicaVoxel's translucent shadows): sunlit
//                              # water without caustic noise, but not exact
//   focus        10            # distance to the plane of perfect focus
//
// The camera line takes any of its settings, in any order; aperture and
// focus can also be given on it.
//
//   texture  earth  image textures/earthmap.jpg   # path relative to the scene file
//   texture  bumps  image textures/bumps_normal.png linear
//                    # linear: 8-bit values are data (k / 255), not sRGB colors;
//                    # use it for normal, roughness and metallic maps
//   texture  tiles  checker 0.5  0.2 0.3 0.1  0.9 0.9 0.9   # cube size, two colors
//   texture  plain  solid 0.8 0.2 0.2
//
//   material ground lambertian 0.8 0.8 0.0    # albedo r g b ...
//   material globe  lambertian texture earth  # ... or a named texture
//   material gold   metal 0.8 0.6 0.2 0.3     # albedo r g b, roughness (0 = mirror)
//   material glass  dielectric 1.5            # index of refraction
//   material frost  dielectric 1.5 roughness 0.3
//   material bottle dielectric 1.5 color 0.6 0.9 0.7 distance 2
//                              # colored glass (Beer-Lambert absorption): light that
//                              # travels <distance> inside keeps <color> of itself,
//                              # so thick parts are darker (defaults: clear, distance 1)
//   material paint  principled color 0.8 0.1 0.1 metallic 0 roughness 0.4 ior 1.5
//
// principled settings are all optional and can come in any order (defaults:
// color 0.8 0.8 0.8, metallic 0, roughness 0.5, ior 1.5); "texture <name>"
// can replace "color r g b", and "metallic texture <name>" and "roughness
// texture <name>" read those from a texture's red channel at every hit:
//   material worn   principled texture rust metallic texture rust_m roughness texture rust_r
//
// lambertian, metal and principled lines can end with a tangent-space normal
// map (an RGB image texture, loaded linear; see normal_mapped in material.h):
//   material bricks lambertian texture brick_color normal_map brick_normal
//   material steel  metal 0.9 0.9 0.9 0.2 normal_map bumps
//
//   normal_maps  off           # ignore normal maps (normal_map and those of OBJ/MTL
//                              # files) on the lines that follow; "on" is the default
//   material lamp   emissive 15 15 15         # area light, radiance r g b
//   material haze   volume 0.9 0.9 0.9 g 0.3  # for media: scattering albedo r g b,
//                                             # optional Henyey-Greenstein g in (-1, 1)
//                                             # (0 = isotropic, > 0 = forward)
//
//   background   0 0 0         # constant background instead of the sky gradient
//   environment  textures/sky.hdr  [intensity 1] [rotate 0]
//                              # equirectangular HDR sky that also lights the scene
//                              # (importance sampled); rotate turns it about y, in degrees
//   sun elevation 50 azimuth 45 [intensity 3] [color 1 1 1] [angle 4] [disk on]
//                              # distant light: elevation above the horizon and
//                              # azimuth (from -z towards +x) in degrees; intensity is
//                              # the irradiance on a surface facing it; angle is its
//                              # angular diameter in degrees (softer shadows when
//                              # larger, same brightness); disk off hides the sun
//                              # itself from the camera. "sun off" removes it
//   sky uniform 0.5 0.6 0.8    # sky of constant radiance (importance sampled like an
//                              # environment); "sky gradient" is the default sky
//   sky atmosphere [rayleigh 1] [mie 1] [mie_g 0.8] [ozone 1] [intensity 3]
//                              # physically based sky lit by the sun's settings
//                              # (see atmosphere.h): densities relative to Earth's;
//                              # the sun's intensity is then above the air, which
//                              # dims and reddens it near the horizon
//   ground [height 0] [material]   # infinite plane y = height (default grey
//                              # lambertian); "ground off" removes it
//   light_selection power      # how next event estimation picks a light: power (the
//                              # default, in proportion to emitted power) or uniform
//   bloom 0.3 [threshold 1] [radius 0.04]   # glow around radiance above the
//                              # threshold, reaching radius x image width; strength
//                              # 0 is off (display images only, like the next ones)
//   vignette     0.3           # darker corners (0 = off)
//   exposure     0             # stops; display images are scaled by 2^exposure
//   tonemap      aces          # none (clamp, the default), reinhard or aces
//
//   sphere       0 0 -1  0.5  glass   # center x y z, radius, [material]
//   quad   Qx Qy Qz  ux uy uz  vx vy vz  [material]   # corner, two edges
//   box    x0 y0 z0  x1 y1 z1  [material]            # two opposite corners
//   mesh   models/bunny.obj  [smooth] [material]      # Wavefront OBJ, path relative
//                                                     # to the scene file; smooth
//                                                     # makes vertex normals if the
//                                                     # file has none
//
//   vox    models/island.vox  [quads] [emission 1]    # MagicaVoxel model or scene:
//                                                     # every model the file's scene
//                                                     # graph places, with its palette
//                                                     # and materials (see voxel.h);
//                                                     # one voxel is one unit, z-up
//                                                     # turned to y-up; emission scales
//                                                     # the emissive voxels; quads uses
//                                                     # faces in a BVH instead of the
//                                                     # voxel grid (same image, slower)
//   vox    models/island.vox  edges 0.06 [0 0 0]      # outlines the voxel shapes
//                                                     # (grid only), width in voxels
//
// Any object can be followed by transforms, applied in the order given:
//
//   box 0 0 0  165 330 165  white  rotate_y 15  translate 265 0 295
//   mesh bunny.obj glass  scale 2000  rotate 30 0 1 0  translate 278 0 278
//
//   translate x y z | scale s | scale sx sy sz | rotate_x|rotate_y|rotate_z deg
//   rotate deg ax ay az   # about an arbitrary axis
//
// Participating media (fog, smoke) fill a sphere or box with a constant
// density (collisions per unit length; light goes 1/density on average
// before scattering). They take a volume material and transforms like other
// objects:
//
//   medium 0.01  box 1 1 1  554 554 554  haze      # density, boundary, material
//   medium 0.05  sphere 0 1 0  1  haze  translate 2 0 0
//   fog    0.002 haze      # fills the bounding box of everything in the scene
//   height_fog 0.02 8 haze [base 0]   # fog everywhere, density 0.02 at y = base,
//                              # falling by e every 8 units up: valley mist, and
//                              # a horizon that fades into the sky
//
// Transformed objects are instances sharing the original geometry; a mesh
// file used several times with the same material is loaded once. In a mesh,
// "usemtl <name>" selects the scene material of that name when there is one,
// else the one from the OBJ's mtllib file (colors, textures, alpha masks;
// see load_mtl in obj_loader.h), else the mesh line's material.
//
// Materials must be defined before they are used. A sphere without a
// material name gets a default grey Lambertian.
//
// Every object with an emissive material is also added to lights, so the
// renderer can sample it directly; lights are picked in proportion to their
// power (see light_list.h) unless light_selection says uniform.
//
// Returns false and prints the offending line on std::cerr on error.
inline bool load_scene(const std::string& path, scene& result) {
    auto& world = result.world;
    auto& lights = result.lights;
    auto& cam = result.cam;

    std::ifstream file(path);
    if (!file) {
        std::cerr << "Error: cannot open scene file '" << path << "'\n";
        return false;
    }

    auto fail = [&](int line_number, const std::string& message) {
        std::cerr << path << ":" << line_number << ": " << message << '\n';
        return false;
    };

    std::map<std::string, std::shared_ptr<material>> materials;
    std::map<std::string, std::shared_ptr<texture>> textures;

    // Directory of the scene file, for resolving relative texture paths.
    auto slash = path.find_last_of('/');
    std::string scene_dir = slash == std::string::npos ? "" : path.substr(0, slash + 1);

    // Reads either "r g b" or "texture <name>" into tex, where first is the
    // token already read ("texture" or the red value); false if neither works.
    auto read_color_or_texture = [&](std::istringstream& in, const std::string& first,
                                     std::shared_ptr<texture>& tex, std::string& error) {
        if (first == "texture") {
            std::string name;
            if (!(in >> name))
                return false;
            auto it = textures.find(name);
            if (it == textures.end()) {
                error = "unknown texture '" + name + "'";
                return false;
            }
            tex = it->second;
            return true;
        }
        std::istringstream rgb(first);
        double r, g, b;
        if (!(rgb >> r) || !(in >> g >> b))
            return false;
        tex = std::make_shared<solid_color>(color(r, g, b));
        return true;
    };
    auto default_material = std::make_shared<lambertian>(color(0.5, 0.5, 0.5));

    // Reads "<name>" of a defined texture into tex; false with a message in error if there is none.
    auto read_texture_name = [&](std::istringstream& in, std::shared_ptr<texture>& tex, std::string& error) {
        std::string name;
        if (!(in >> name)) {
            error = "expected a texture name";
            return false;
        }
        auto it = textures.find(name);
        if (it == textures.end()) {
            error = "unknown texture '" + name + "'";
            return false;
        }
        tex = it->second;
        return true;
    };

    // Reads "<number>" or "texture <name>" into tex, the number within [0, 1]
    // (a principled metallic or roughness value).
    auto read_value_or_texture = [&](std::istringstream& in, std::shared_ptr<texture>& tex, std::string& error) {
        std::string first;
        if (!(in >> first))
            return false;
        if (first == "texture")
            return read_texture_name(in, tex, error);
        std::istringstream number(first);
        double x;
        if (!(number >> x) || x < 0 || x > 1)
            return false;
        tex = std::make_shared<solid_color>(color(x, x, x));
        return true;
    };

    // Ignores normal maps after "normal_maps off", to compare renders
    bool use_normal_maps = true;

    // Reads what may follow an object's geometry: an optional material name,
    // then any transforms, applied in the order given. Returns false with a
    // message in error on a problem.
    auto read_tail = [&](std::istringstream& in, std::shared_ptr<material>& mat, transform& xform,
                         bool& transformed, std::string& error) {
        mat = default_material;
        xform = transform();
        transformed = false;

        auto is_transform = [](const std::string& t) {
            return t == "translate" || t == "scale" || t == "rotate"
                || t == "rotate_x" || t == "rotate_y" || t == "rotate_z";
        };

        std::string token;
        if (!(in >> token))
            return true;
        if (!is_transform(token)) {
            auto it = materials.find(token);
            if (it == materials.end()) {
                error = "unknown material '" + token + "'";
                return false;
            }
            mat = it->second;
            if (!(in >> token))
                return true;
        }

        do {
            transform step;
            bool ok = true;
            if (token == "translate") {
                vec3 d;
                ok = bool(in >> d[0] >> d[1] >> d[2]);
                step = transform::translate(d);
            } else if (token == "rotate_x" || token == "rotate_y" || token == "rotate_z") {
                double degrees;
                ok = bool(in >> degrees);
                vec3 axis(token == "rotate_x", token == "rotate_y", token == "rotate_z");
                step = transform::rotate(degrees, axis);
            } else if (token == "rotate") {
                double degrees;
                vec3 axis;
                ok = bool(in >> degrees >> axis[0] >> axis[1] >> axis[2]) && axis.length_squared() > 0;
                if (ok)
                    step = transform::rotate(degrees, axis);
            } else if (token == "scale") {
                // "scale s" or "scale sx sy sz"
                vec3 f;
                ok = bool(in >> f[0]);
                if (ok) {
                    // A second number means all three must be there
                    std::streampos mark = in.tellg();
                    if (in >> f[1]) {
                        ok = bool(in >> f[2]);
                    } else {
                        in.clear();
                        in.seekg(mark);
                        f = vec3(f[0], f[0], f[0]);
                    }
                }
                ok = ok && f[0] != 0 && f[1] != 0 && f[2] != 0;
                step = transform::scale(f);
            } else {
                error = "unexpected value '" + token + "' (expected a transform: translate, scale, "
                        "rotate, rotate_x, rotate_y or rotate_z)";
                return false;
            }
            if (!ok) {
                error = "bad values for '" + token + "'";
                return false;
            }
            xform = step.then_after(xform);
            transformed = true;
        } while (in >> token);
        return true;
    };

    // Adds an object to the world, and to the lights if it emits.
    auto add_object = [&](std::shared_ptr<hittable> object, const std::shared_ptr<material>& mat,
                          const transform& xform, bool transformed, std::string& error) {
        bool emissive = bool(std::dynamic_pointer_cast<diffuse_light>(mat));
        if (transformed) {
            if (emissive && !xform.is_similarity()) {
                error = "lights can only be moved, rotated and scaled uniformly";
                return false;
            }
            object = std::make_shared<instance>(object, xform);
        }
        world.add(object);
        if (emissive)
            lights.add(object);
        return true;
    };

    // The sun, sky and ground are created at the end of the file, so later
    // lines can change what earlier ones set
    class sun_settings{
        public:
            bool   on = false;
            double elevation = 45, azimuth = 45;  // Degrees
            double intensity = 3;                 // Irradiance
            color  tint = color(1, 1, 1);
            double angle = 4;                     // Angular diameter, degrees
            bool   disk = true;
    } sun;
    bool   sky_uniform = false;
    color  sky_radiance;
    bool   sky_atmosphere = false;
    atmosphere_settings air;
    bool   ground = false;
    double ground_height = 0;
    std::shared_ptr<material> ground_material = default_material;

    // Meshes already loaded, by file and default material, so repeated
    // "mesh" lines share geometry and only add instances.
    std::map<std::tuple<std::string, bool, const material*, bool>, std::vector<obj_part>> mesh_cache;

    // Global fog, added once the whole scene (and so its extent) is known
    double fog_density = 0;
    std::shared_ptr<material> fog_material;

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        line_number++;

        auto comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);

        std::istringstream in(line);
        std::string keyword;
        if (!(in >> keyword))
            continue;  // Empty line

        if (keyword == "image_width") {
            if (!(in >> cam.image_width) || cam.image_width < 1)
                return fail(line_number, "expected 'image_width <positive integer>'");
        } else if (keyword == "aspect_ratio") {
            // Accepts "16/9" as well as a plain number
            double num, den = 1.0;
            if (!(in >> num))
                return fail(line_number, "expected 'aspect_ratio <w>/<h>' or 'aspect_ratio <number>'");
            if (in.peek() == '/') {
                in.get();
                if (!(in >> den))
                    return fail(line_number, "expected 'aspect_ratio <w>/<h>'");
            }
            if (num <= 0 || den <= 0)
                return fail(line_number, "aspect_ratio must be positive");
            cam.aspect_ratio = num / den;
        } else if (keyword == "samples_per_pixel") {
            if (!(in >> cam.samples_per_pixel) || cam.samples_per_pixel < 1)
                return fail(line_number, "expected 'samples_per_pixel <positive integer>'");
        } else if (keyword == "max_depth") {
            if (!(in >> cam.max_depth) || cam.max_depth < 1)
                return fail(line_number, "expected 'max_depth <positive integer>'");
        } else if (keyword == "camera") {
            std::string key;
            while (in >> key) {
                bool ok = true;
                if (key == "lookfrom")      ok = bool(in >> cam.lookfrom[0] >> cam.lookfrom[1] >> cam.lookfrom[2]);
                else if (key == "lookat")   ok = bool(in >> cam.lookat[0] >> cam.lookat[1] >> cam.lookat[2]);
                else if (key == "vup")      ok = bool(in >> cam.vup[0] >> cam.vup[1] >> cam.vup[2]);
                else if (key == "vfov")     ok = bool(in >> cam.vfov) && cam.vfov > 0 && cam.vfov < 180;
                else if (key == "aperture") ok = bool(in >> cam.defocus_angle) && cam.defocus_angle >= 0;
                else if (key == "focus")    ok = bool(in >> cam.focus_dist) && cam.focus_dist > 0;
                else if (key == "ortho")    ok = bool(in >> cam.ortho_height) && cam.ortho_height >= 0;
                else if (key == "blades")   ok = bool(in >> cam.blades) && (cam.blades == 0 || cam.blades >= 3);
                else if (key == "blade_rotation") ok = bool(in >> cam.blade_rotation);
                else return fail(line_number, "unknown camera setting '" + key + "'");
                if (!ok)
                    return fail(line_number, "bad value for camera setting '" + key + "'");
            }
        } else if (keyword == "vfov") {
            if (!(in >> cam.vfov) || cam.vfov <= 0 || cam.vfov >= 180)
                return fail(line_number, "expected 'vfov <degrees between 0 and 180>'");
        } else if (keyword == "aperture") {
            if (!(in >> cam.defocus_angle) || cam.defocus_angle < 0)
                return fail(line_number, "expected 'aperture <defocus angle in degrees>'");
        } else if (keyword == "focus") {
            if (!(in >> cam.focus_dist) || cam.focus_dist <= 0)
                return fail(line_number, "expected 'focus <positive distance>'");
        } else if (keyword == "background") {
            double r, g, b;
            if (!(in >> r >> g >> b))
                return fail(line_number, "expected 'background <r> <g> <b>'");
            cam.use_sky = false;
            cam.background = color(r, g, b);
        } else if (keyword == "light_selection") {
            std::string mode;
            if (!(in >> mode) || (mode != "power" && mode != "uniform"))
                return fail(line_number, "expected 'light_selection power|uniform'");
            lights.strategy = mode == "power" ? light_list::selection::power : light_list::selection::uniform;
        } else if (keyword == "environment") {
            std::string file, key;
            double intensity = 1, rotation = 0;
            const char* usage = "expected 'environment <file.hdr> [intensity <s>] [rotate <degrees>]'";
            if (!(in >> file))
                return fail(line_number, usage);
            while (in >> key) {
                bool ok = (key == "intensity" && in >> intensity && intensity >= 0)
                       || (key == "rotate" && in >> rotation);
                if (!ok)
                    return fail(line_number, usage);
            }
            if (file[0] != '/')
                file = scene_dir + file;
            auto sky = std::make_shared<environment_light>(file, intensity, rotation);
            if (!sky->loaded())
                return fail(line_number, "cannot load environment map '" + file + "'");
            if (cam.environment)
                return fail(line_number, "only one environment is allowed");
            cam.environment = sky;
            lights.add(sky);  // Sampled by next event estimation; never hit, so not in world
        } else if (keyword == "sun") {
            const char* usage = "expected 'sun elevation <deg> azimuth <deg> [intensity <E>] [color <r g b>] "
                                "[angle <deg>] [disk on|off]' or 'sun off'";
            std::string key;
            sun.on = true;
            while (in >> key) {
                bool ok = true;
                if (key == "off")            sun.on = false;
                else if (key == "elevation") ok = bool(in >> sun.elevation) && sun.elevation >= -90 && sun.elevation <= 90;
                else if (key == "azimuth")   ok = bool(in >> sun.azimuth);
                else if (key == "intensity") ok = bool(in >> sun.intensity) && sun.intensity >= 0;
                else if (key == "color")     ok = bool(in >> sun.tint[0] >> sun.tint[1] >> sun.tint[2]);
                else if (key == "angle")     ok = bool(in >> sun.angle) && sun.angle > 0 && sun.angle < 90;
                else if (key == "disk") {
                    std::string value;
                    ok = bool(in >> value) && (value == "on" || value == "off");
                    sun.disk = value == "on";
                } else ok = false;
                if (!ok)
                    return fail(line_number, usage);
            }
        } else if (keyword == "sky") {
            std::string type;
            if (!(in >> type))
                return fail(line_number, "expected 'sky uniform <r g b>', 'sky atmosphere [...]' or 'sky gradient'");
            if (type == "uniform") {
                if (!(in >> sky_radiance[0] >> sky_radiance[1] >> sky_radiance[2]))
                    return fail(line_number, "expected 'sky uniform <r g b>'");
                sky_uniform = true;
                cam.use_sky = true;
                sky_atmosphere = false;
            } else if (type == "atmosphere") {
                const char* usage = "expected 'sky atmosphere [rayleigh <x>] [mie <x>] [mie_g <g>] [ozone <x>] [intensity <s>]'";
                std::string key;
                while (in >> key) {
                    bool ok = (key == "rayleigh" && in >> air.rayleigh && air.rayleigh >= 0)
                           || (key == "mie" && in >> air.mie && air.mie >= 0)
                           || (key == "mie_g" && in >> air.mie_g && air.mie_g > -1 && air.mie_g < 1)
                           || (key == "ozone" && in >> air.ozone && air.ozone >= 0)
                           || (key == "intensity" && in >> air.intensity && air.intensity >= 0);
                    if (!ok)
                        return fail(line_number, usage);
                }
                sky_atmosphere = true;
                sky_uniform = false;
                cam.use_sky = true;
            } else if (type == "gradient") {
                sky_uniform = false;
                sky_atmosphere = false;
                cam.use_sky = true;
            } else {
                return fail(line_number, "unknown sky type '" + type + "' (expected uniform, atmosphere or gradient)");
            }
        } else if (keyword == "ground") {
            std::string key;
            ground = true;
            while (in >> key) {
                if (key == "off") {
                    ground = false;
                } else if (key == "height") {
                    if (!(in >> ground_height))
                        return fail(line_number, "expected 'ground [height <y>] [material]'");
                } else {
                    auto it = materials.find(key);
                    if (it == materials.end())
                        return fail(line_number, "unknown material '" + key + "'");
                    ground_material = it->second;
                }
            }
        } else if (keyword == "bounces") {
            const char* usage = "expected 'bounces [diffuse <n>] [specular <n>] [scatter <n>]'";
            std::string key;
            while (in >> key) {
                bool ok = (key == "diffuse" && in >> cam.max_diffuse && cam.max_diffuse >= 0)
                       || (key == "specular" && in >> cam.max_specular && cam.max_specular >= 0)
                       || (key == "scatter" && in >> cam.max_scatter && cam.max_scatter >= 0);
                if (!ok)
                    return fail(line_number, usage);
            }
        } else if (keyword == "transparent_shadows") {
            std::string value;
            if (!(in >> value) || (value != "on" && value != "off"))
                return fail(line_number, "expected 'transparent_shadows on|off'");
            cam.transparent_shadows = value == "on";
        } else if (keyword == "height_fog") {
            double density, scale, base = 0;
            std::string name, key;
            if (!(in >> density >> scale >> name) || density <= 0 || scale <= 0)
                return fail(line_number, "expected 'height_fog <density> <scale height> <volume material> [base <y>]'");
            if (in >> key && (key != "base" || !(in >> base)))
                return fail(line_number, "expected 'height_fog <density> <scale height> <volume material> [base <y>]'");
            auto it = materials.find(name);
            if (it == materials.end() || !std::dynamic_pointer_cast<phase_material>(it->second))
                return fail(line_number, "expected a volume material, got '" + name + "'");
            result.unbounded.add(std::make_shared<height_fog>(density, scale, base, it->second));
        } else if (keyword == "bloom") {
            auto& post = result.display.post;
            const char* usage = "expected 'bloom <strength> [threshold <radiance>] [radius <fraction of width>]'";
            std::string key;
            if (!(in >> post.bloom) || post.bloom < 0)
                return fail(line_number, usage);
            while (in >> key) {
                bool ok = (key == "threshold" && in >> post.bloom_threshold && post.bloom_threshold >= 0)
                       || (key == "radius" && in >> post.bloom_radius && post.bloom_radius > 0);
                if (!ok)
                    return fail(line_number, usage);
            }
        } else if (keyword == "vignette") {
            if (!(in >> result.display.post.vignette) || result.display.post.vignette < 0)
                return fail(line_number, "expected 'vignette <strength>'");
        } else if (keyword == "exposure") {
            if (!(in >> result.display.exposure))
                return fail(line_number, "expected 'exposure <stops>'");
        } else if (keyword == "tonemap") {
            std::string name;
            if (!(in >> name) || !result.display.set_tonemapper(name))
                return fail(line_number, "expected 'tonemap none|reinhard|aces'");
        } else if (keyword == "normal_maps") {
            std::string state;
            if (!(in >> state) || (state != "on" && state != "off"))
                return fail(line_number, "expected 'normal_maps on|off'");
            use_normal_maps = state == "on";
        } else if (keyword == "texture") {
            std::string name, type;
            if (!(in >> name >> type))
                return fail(line_number, "expected 'texture <name> <type> <parameters...>'");

            std::shared_ptr<texture> tex;
            if (type == "solid") {
                double r, g, b;
                if (!(in >> r >> g >> b))
                    return fail(line_number, "expected 'texture <name> solid <r> <g> <b>'");
                tex = std::make_shared<solid_color>(color(r, g, b));
            } else if (type == "checker") {
                double scale, r0, g0, b0, r1, g1, b1;
                if (!(in >> scale >> r0 >> g0 >> b0 >> r1 >> g1 >> b1) || scale <= 0)
                    return fail(line_number, "expected 'texture <name> checker <scale> <r g b> <r g b>'");
                tex = std::make_shared<checker_texture>(scale,
                    std::make_shared<solid_color>(color(r0, g0, b0)),
                    std::make_shared<solid_color>(color(r1, g1, b1)));
            } else if (type == "image") {
                std::string file, option;
                if (!(in >> file))
                    return fail(line_number, "expected 'texture <name> image <path> [linear]'");
                bool linear = false;
                if (in >> option) {
                    if (option != "linear")
                        return fail(line_number, "expected 'texture <name> image <path> [linear]'");
                    linear = true;
                }
                if (file[0] != '/')
                    file = scene_dir + file;
                auto image = std::make_shared<image_texture>(file, false, linear);
                if (!image->loaded())
                    return fail(line_number, "cannot load image '" + file + "'");
                tex = image;
            } else {
                return fail(line_number, "unknown texture type '" + type + "'");
            }
            textures[name] = tex;
        } else if (keyword == "material") {
            std::string name, type;
            if (!(in >> name >> type))
                return fail(line_number, "expected 'material <name> <type> <parameters...>'");

            std::shared_ptr<material> mat;
            std::shared_ptr<texture> normal_map;  // Optional, for lambertian, metal and principled
            // An optional "normal_map <texture>" at the end of the line
            auto read_normal_map = [&](std::string& error) {
                std::string key;
                if (!(in >> key))
                    return true;
                if (key != "normal_map") {
                    error = "unexpected value '" + key + "' (expected 'normal_map <texture>')";
                    return false;
                }
                return read_texture_name(in, normal_map, error);
            };
            if (type == "lambertian") {
                std::shared_ptr<texture> tex;
                std::string first, error;
                if (!(in >> first) || !read_color_or_texture(in, first, tex, error))
                    return fail(line_number, error.empty()
                        ? "expected 'material <name> lambertian <r> <g> <b>' or '... lambertian texture <name>'"
                        : error);
                if (!read_normal_map(error))
                    return fail(line_number, error);
                mat = std::make_shared<lambertian>(tex);
            } else if (type == "metal") {
                double r, g, b, roughness;
                std::string error;
                if (!(in >> r >> g >> b >> roughness) || roughness < 0)
                    return fail(line_number, "expected 'material <name> metal <r> <g> <b> <roughness>'");
                if (!read_normal_map(error))
                    return fail(line_number, error);
                mat = std::make_shared<metal>(color(r, g, b), roughness);
            } else if (type == "dielectric") {
                const char* usage = "expected 'material <name> dielectric <index of refraction>"
                                    " [roughness <r>] [color <r> <g> <b> [distance <d>]]'";
                double ior, roughness = 0, distance = 1;
                color tint(1, 1, 1);
                std::string key;
                if (!(in >> ior) || ior <= 0)
                    return fail(line_number, usage);
                while (in >> key) {
                    bool ok = true;
                    if (key == "roughness") {
                        ok = bool(in >> roughness) && roughness >= 0;
                    } else if (key == "color") {
                        double r, g, b;
                        // 0 would need infinite absorption, > 1 would add energy
                        ok = bool(in >> r >> g >> b) && r > 0 && g > 0 && b > 0 && r <= 1 && g <= 1 && b <= 1;
                        tint = color(r, g, b);
                    } else if (key == "distance") {
                        ok = bool(in >> distance) && distance > 0;
                    } else {
                        ok = false;
                    }
                    if (!ok)
                        return fail(line_number, usage);
                }
                mat = std::make_shared<dielectric>(ior, roughness, dielectric::absorption_for(tint, distance));
            } else if (type == "principled") {
                std::shared_ptr<texture> base = std::make_shared<solid_color>(color(0.8, 0.8, 0.8));
                std::shared_ptr<texture> metallic = std::make_shared<solid_color>(color(0, 0, 0));
                std::shared_ptr<texture> roughness = std::make_shared<solid_color>(color(0.5, 0.5, 0.5));
                double ior = 1.5;
                std::string key, error;
                while (in >> key) {
                    bool ok = true;
                    if (key == "texture") {
                        ok = read_color_or_texture(in, key, base, error);
                    } else if (key == "color") {
                        std::string first;
                        ok = bool(in >> first) && read_color_or_texture(in, first, base, error);
                    }
                    else if (key == "metallic")   ok = read_value_or_texture(in, metallic, error);
                    else if (key == "roughness")  ok = read_value_or_texture(in, roughness, error);
                    else if (key == "ior")        ok = bool(in >> ior) && ior > 0;
                    else if (key == "normal_map") ok = read_texture_name(in, normal_map, error);
                    else return fail(line_number, "unknown principled setting '" + key + "'");
                    if (!ok)
                        return fail(line_number, error.empty() ? "bad value for principled setting '" + key + "'" : error);
                }
                mat = std::make_shared<principled>(base, metallic, roughness, ior);
            } else if (type == "volume") {
                double r, g, b, aniso = 0;
                std::string key;
                if (!(in >> r >> g >> b))
                    return fail(line_number, "expected 'material <name> volume <r> <g> <b> [g <anisotropy>]'");
                if (in >> key) {
                    if (key != "g" || !(in >> aniso) || aniso <= -1 || aniso >= 1)
                        return fail(line_number, "expected 'material <name> volume <r> <g> <b> [g <anisotropy between -1 and 1>]'");
                }
                mat = std::make_shared<phase_material>(color(r, g, b), aniso);
            } else if (type == "emissive") {
                double r, g, b;
                if (!(in >> r >> g >> b))
                    return fail(line_number, "expected 'material <name> emissive <r> <g> <b>'");
                mat = std::make_shared<diffuse_light>(color(r, g, b));
            } else {
                return fail(line_number, "unknown material type '" + type + "'");
            }
            if (normal_map && use_normal_maps)
                mat = std::make_shared<normal_mapped>(mat, normal_map);
            materials[name] = mat;
        } else if (keyword == "sphere" || keyword == "quad" || keyword == "box") {
            std::shared_ptr<hittable> object;
            std::shared_ptr<material> mat;
            transform xform;
            bool transformed;
            std::string error;

            if (keyword == "sphere") {
                double x, y, z, radius;
                if (!(in >> x >> y >> z >> radius) || radius == 0)
                    return fail(line_number, "expected 'sphere <x> <y> <z> <non-zero radius> [material] [transforms]'");
                if (!read_tail(in, mat, xform, transformed, error))
                    return fail(line_number, error);
                object = std::make_shared<sphere>(point3(x, y, z), radius, mat);
            } else if (keyword == "quad") {
                point3 Q; vec3 u, v;
                if (!(in >> Q[0] >> Q[1] >> Q[2] >> u[0] >> u[1] >> u[2] >> v[0] >> v[1] >> v[2]))
                    return fail(line_number, "expected 'quad <Qx Qy Qz> <ux uy uz> <vx vy vz> [material] [transforms]'");
                if (cross(u, v).near_zero())
                    return fail(line_number, "quad edges must not be parallel");
                if (!read_tail(in, mat, xform, transformed, error))
                    return fail(line_number, error);
                object = std::make_shared<quad>(Q, u, v, mat);
            } else {
                point3 a, b;
                if (!(in >> a[0] >> a[1] >> a[2] >> b[0] >> b[1] >> b[2]))
                    return fail(line_number, "expected 'box <x0 y0 z0> <x1 y1 z1> [material] [transforms]'");
                if (!read_tail(in, mat, xform, transformed, error))
                    return fail(line_number, error);
                object = box(a, b, mat);
            }
            if (!add_object(object, mat, xform, transformed, error))
                return fail(line_number, error);
        } else if (keyword == "medium") {
            double density;
            std::string shape, error;
            std::shared_ptr<hittable> boundary;
            std::shared_ptr<material> mat;
            transform xform;
            bool transformed;
            if (!(in >> density >> shape) || density <= 0)
                return fail(line_number, "expected 'medium <positive density> sphere|box <...> <material> [transforms]'");
            if (shape == "sphere") {
                double x, y, z, radius;
                if (!(in >> x >> y >> z >> radius) || radius <= 0)
                    return fail(line_number, "expected 'medium <density> sphere <x> <y> <z> <positive radius> <material>'");
                boundary = std::make_shared<sphere>(point3(x, y, z), radius, default_material);
            } else if (shape == "box") {
                point3 a, b;
                if (!(in >> a[0] >> a[1] >> a[2] >> b[0] >> b[1] >> b[2]))
                    return fail(line_number, "expected 'medium <density> box <x0 y0 z0> <x1 y1 z1> <material>'");
                boundary = box(a, b, default_material);
            } else {
                return fail(line_number, "a medium's boundary must be 'sphere' or 'box'");
            }
            if (!read_tail(in, mat, xform, transformed, error))
                return fail(line_number, error);
            if (!std::dynamic_pointer_cast<phase_material>(mat))
                return fail(line_number, "a medium needs a volume material");
            // The boundary is transformed, not the medium, so densities stay in world units
            if (transformed)
                boundary = std::make_shared<instance>(boundary, xform);
            world.add(std::make_shared<constant_medium>(boundary, density, mat));
        } else if (keyword == "fog") {
            std::string name;
            if (!(in >> fog_density >> name) || fog_density <= 0)
                return fail(line_number, "expected 'fog <positive density> <volume material>'");
            auto it = materials.find(name);
            if (it == materials.end() || !std::dynamic_pointer_cast<phase_material>(it->second))
                return fail(line_number, "expected a volume material, got '" + name + "'");
            fog_material = it->second;
        } else if (keyword == "vox") {
            const char* usage = "expected 'vox <file.vox> [quads] [emission <scale>] [edges <width> [<r g b>]] [transforms]'";
            std::string file, token, error;
            if (!(in >> file))
                return fail(line_number, usage);
            if (file[0] != '/')
                file = scene_dir + file;
            // Options come first; the rest are transforms
            bool quads = false;
            double emission = 1, edge_width = 0;
            color edge_color(0, 0, 0);
            std::vector<std::string> rest;
            while (in >> token)
                rest.push_back(token);
            size_t k = 0;
            for (; k < rest.size(); k++) {
                if (rest[k] == "quads") {
                    quads = true;
                } else if (rest[k] == "emission" && k + 1 < rest.size()) {
                    std::istringstream number(rest[++k]);
                    if (!(number >> emission) || emission < 0)
                        return fail(line_number, usage);
                } else if (rest[k] == "edges" && k + 1 < rest.size()) {
                    // edges <width> [r g b]
                    std::istringstream number(rest[++k]);
                    if (!(number >> edge_width) || edge_width <= 0 || edge_width >= 0.5)
                        return fail(line_number, usage);
                    if (k + 3 < rest.size()) {
                        std::istringstream rgb(rest[k + 1] + ' ' + rest[k + 2] + ' ' + rest[k + 3]);
                        color c;
                        if (rgb >> c[0] >> c[1] >> c[2]) {
                            edge_color = c;
                            k += 3;
                        }
                    }
                } else {
                    break;
                }
            }
            std::string tail_text;
            for (; k < rest.size(); k++)
                tail_text += rest[k] + ' ';
            std::istringstream tail(tail_text);
            std::shared_ptr<material> mat;
            transform xform;
            bool transformed;
            if (!read_tail(tail, mat, xform, transformed, error))
                return fail(line_number, error);
            if (mat != default_material)
                return fail(line_number, "vox models take their materials from the file's palette");

            vox_file vox;
            if (!load_vox(file, vox, error))
                return fail(line_number, error);
            auto palette = std::make_shared<voxel_palette>(make_voxel_palette(vox, emission));

            // Geometry per model, shared by the instances that place it: a
            // voxel grid, or with "quads" its visible faces in a BVH (slower,
            // kept as a reference). Emissive faces are lights either way;
            // with a grid they are only sampled, the grid itself is hit.
            std::vector<std::shared_ptr<hittable>> geometry(vox.models.size());
            std::vector<std::shared_ptr<hittable_list>> emitters(vox.models.size());
            size_t faces = 0;
            for (size_t m = 0; m < vox.models.size(); m++) {
                hittable_list list;
                emitters[m] = std::make_shared<hittable_list>();
                voxel_model_faces(vox.models[m], *palette, list, *emitters[m]);
                faces += list.objects.size();
                if (quads)
                    geometry[m] = std::make_shared<bvh>(list);
                else
                    geometry[m] = voxel_blocks(std::make_shared<vox_model>(vox.models[m]), palette, 8,
                                               edge_width > 0 ? std::make_shared<lambertian>(edge_color) : nullptr,
                                               edge_width);
            }
            std::clog << "Loaded " << file << ": " << vox.models.size() << " models, "
                      << vox.instances.size() << " instances, " << faces << " visible faces\n";

            // MagicaVoxel is z-up: turn its +z to mypbr's +y (and +y to -z)
            transform z_up = transform::rotate(-90, vec3(1, 0, 0));
            for (const auto& placed : vox.instances) {
                transform to_world = xform.then_after(z_up.then_after(placed.xform));
                world.add(std::make_shared<instance>(geometry[placed.model], to_world));
                if (!emitters[placed.model]->empty()) {
                    if (!to_world.is_similarity())
                        return fail(line_number, "a vox model with emissive voxels can only be scaled uniformly");
                    lights.add(std::make_shared<instance>(emitters[placed.model], to_world));
                }
            }
        } else if (keyword == "mesh") {
            std::string file, error;
            std::shared_ptr<material> mat;
            transform xform;
            bool transformed;
            if (!(in >> file))
                return fail(line_number, "expected 'mesh <file.obj> [material] [transforms]'");
            if (file[0] != '/')
                file = scene_dir + file;
            // An optional "smooth" comes before the usual material and transforms
            std::string rest, first;
            std::getline(in, rest);
            std::istringstream peek(rest);
            bool smooth = (peek >> first) && first == "smooth";
            std::istringstream tail(smooth ? rest.substr(rest.find("smooth") + 6) : rest);
            if (!read_tail(tail, mat, xform, transformed, error))
                return fail(line_number, error);

            auto key = std::make_tuple(file, smooth, mat.get(), use_normal_maps);
            auto cached = mesh_cache.find(key);
            if (cached == mesh_cache.end()) {
                std::vector<obj_part> parts;
                if (!load_obj(file, materials, mat, smooth, parts, error, use_normal_maps))
                    return fail(line_number, error);
                size_t triangle_count = 0;
                for (const auto& part : parts)
                    triangle_count += part.geometry->size();
                std::clog << "Loaded " << file << ": " << triangle_count << " triangles\n";
                cached = mesh_cache.emplace(key, std::move(parts)).first;
            }
            for (const auto& part : cached->second)
                if (!add_object(part.geometry, part.mat, xform, transformed, error))
                    return fail(line_number, error);
        } else {
            return fail(line_number, "unknown keyword '" + keyword + "'");
        }

        std::string extra;
        if (in >> extra)
            return fail(line_number, "unexpected value '" + extra + "'");
    }

    if (world.empty())
        return fail(line_number, "the scene has no objects");

    if (sky_uniform) {
        if (cam.environment)
            return fail(line_number, "'sky uniform' and 'environment' can't be used together");
        // A tiny constant image: importance sampling then reduces to uniform
        // sampling over the sphere
        cam.environment = std::make_shared<environment_light>(8, 4, std::vector<color>(32, sky_radiance));
        lights.add(cam.environment);
    }
    double sun_el = degrees_to_radians(sun.elevation), sun_az = degrees_to_radians(sun.azimuth);
    vec3 sun_direction(std::cos(sun_el) * std::sin(sun_az), std::sin(sun_el), -std::cos(sun_el) * std::cos(sun_az));
    color sun_irradiance = sun.intensity * sun.tint;
    if (sky_atmosphere) {
        if (cam.environment)
            return fail(line_number, "'sky atmosphere' and 'environment' can't be used together");
        // The sky is lit by the sun's settings even with "sun off", which
        // only removes the sun itself; the sun's light reaches the ground
        // through the air, reddened by it
        atmosphere air_model(air);
        const int w = 256, h = 128;
        cam.environment = std::make_shared<environment_light>(w, h, air_model.bake(w, h, sun_direction, sun_irradiance));
        lights.add(cam.environment);
        sun_irradiance = sun_irradiance * air_model.sun_transmittance(sun_direction);
    }
    if (sun.on) {
        cam.sun = std::make_shared<sun_light>(sun_direction, sun.angle, sun_irradiance);
        cam.sun_disk_visible = sun.disk;
        lights.add(cam.sun);
    }
    if (ground)
        result.unbounded.add(std::make_shared<ground_plane>(ground_height, ground_material));
    if (fog_material) {
        auto extent = world.bounding_box();
        point3 lo(extent.x.min, extent.y.min, extent.z.min);
        point3 hi(extent.x.max, extent.y.max, extent.z.max);
        world.add(std::make_shared<constant_medium>(box(lo, hi, default_material), fog_density, fog_material));
    }
    if ((cam.lookfrom - cam.lookat).near_zero() || cross(cam.vup, cam.lookfrom - cam.lookat).near_zero())
        return fail(line_number, "camera lookfrom must differ from lookat, and vup must not point along the view");

    lights.build(world.bounding_box());
    return true;
}

#endif
