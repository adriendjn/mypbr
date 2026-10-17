#ifndef VOX_LOADER_H
#define VOX_LOADER_H

#include "color3.h"
#include "transform.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Reader for MagicaVoxel .vox files (versions 150 and 200).
//
// A file is a RIFF-style tree of chunks: a 4-byte id, the size of the
// chunk's own content, the size of its children, then both. Under the root
// MAIN chunk come, in order:
//
//   SIZE + XYZI  one pair per model: dimensions, then (x, y, z, index) bytes
//   RGBA         256 colors; color k is palette index k + 1 (0 means empty)
//   MATL         the material of one palette index (a string dictionary)
//   nTRN/nGRP/nSHP  the scene graph placing models with transforms
//
// Other chunks (layers, render settings, cameras, notes) are skipped. The
// format is described in ephtracy/voxel-model on GitHub
// (MagicaVoxel-file-format-vox.txt and -extension.txt).
//
// MagicaVoxel is z-up; the loader keeps its coordinates, and the scene
// loader turns them to y-up when it places the models.

// One model: a dense grid of palette indices, 0 = empty.
class vox_model{
    public:
        int nx = 0, ny = 0, nz = 0;
        std::vector<uint8_t> cells;  // x fastest, then y, then z

        uint8_t at(int x, int y, int z) const {
            if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz)
                return 0;
            return cells[(size_t(z) * ny + y) * nx + x];
        }
};

// A model placed in the file's world by the scene graph. xform takes model
// coordinates (a cell (x, y, z) covers [x, x+1] x [y, y+1] x [z, z+1]) to
// MagicaVoxel's world coordinates, still z-up.
class vox_instance{
    public:
        int model;
        transform xform;
};

class vox_file{
    public:
        std::vector<vox_model> models;
        std::vector<vox_instance> instances;
        color palette[256];  // sRGB-encoded colors in [0, 1], as stored
        std::map<std::string, std::string> materials[256];  // MATL properties per index
};

// MagicaVoxel's default palette (0xAABBGGRR), for files without an RGBA chunk.
inline const uint32_t vox_default_palette[256] = {
    0x00000000, 0xffffffff, 0xffccffff, 0xff99ffff, 0xff66ffff, 0xff33ffff, 0xff00ffff, 0xffffccff, 0xffccccff, 0xff99ccff, 0xff66ccff, 0xff33ccff, 0xff00ccff, 0xffff99ff, 0xffcc99ff, 0xff9999ff,
    0xff6699ff, 0xff3399ff, 0xff0099ff, 0xffff66ff, 0xffcc66ff, 0xff9966ff, 0xff6666ff, 0xff3366ff, 0xff0066ff, 0xffff33ff, 0xffcc33ff, 0xff9933ff, 0xff6633ff, 0xff3333ff, 0xff0033ff, 0xffff00ff,
    0xffcc00ff, 0xff9900ff, 0xff6600ff, 0xff3300ff, 0xff0000ff, 0xffffffcc, 0xffccffcc, 0xff99ffcc, 0xff66ffcc, 0xff33ffcc, 0xff00ffcc, 0xffffcccc, 0xffcccccc, 0xff99cccc, 0xff66cccc, 0xff33cccc,
    0xff00cccc, 0xffff99cc, 0xffcc99cc, 0xff9999cc, 0xff6699cc, 0xff3399cc, 0xff0099cc, 0xffff66cc, 0xffcc66cc, 0xff9966cc, 0xff6666cc, 0xff3366cc, 0xff0066cc, 0xffff33cc, 0xffcc33cc, 0xff9933cc,
    0xff6633cc, 0xff3333cc, 0xff0033cc, 0xffff00cc, 0xffcc00cc, 0xff9900cc, 0xff6600cc, 0xff3300cc, 0xff0000cc, 0xffffff99, 0xffccff99, 0xff99ff99, 0xff66ff99, 0xff33ff99, 0xff00ff99, 0xffffcc99,
    0xffcccc99, 0xff99cc99, 0xff66cc99, 0xff33cc99, 0xff00cc99, 0xffff9999, 0xffcc9999, 0xff999999, 0xff669999, 0xff339999, 0xff009999, 0xffff6699, 0xffcc6699, 0xff996699, 0xff666699, 0xff336699,
    0xff006699, 0xffff3399, 0xffcc3399, 0xff993399, 0xff663399, 0xff333399, 0xff003399, 0xffff0099, 0xffcc0099, 0xff990099, 0xff660099, 0xff330099, 0xff000099, 0xffffff66, 0xffccff66, 0xff99ff66,
    0xff66ff66, 0xff33ff66, 0xff00ff66, 0xffffcc66, 0xffcccc66, 0xff99cc66, 0xff66cc66, 0xff33cc66, 0xff00cc66, 0xffff9966, 0xffcc9966, 0xff999966, 0xff669966, 0xff339966, 0xff009966, 0xffff6666,
    0xffcc6666, 0xff996666, 0xff666666, 0xff336666, 0xff006666, 0xffff3366, 0xffcc3366, 0xff993366, 0xff663366, 0xff333366, 0xff003366, 0xffff0066, 0xffcc0066, 0xff990066, 0xff660066, 0xff330066,
    0xff000066, 0xffffff33, 0xffccff33, 0xff99ff33, 0xff66ff33, 0xff33ff33, 0xff00ff33, 0xffffcc33, 0xffcccc33, 0xff99cc33, 0xff66cc33, 0xff33cc33, 0xff00cc33, 0xffff9933, 0xffcc9933, 0xff999933,
    0xff669933, 0xff339933, 0xff009933, 0xffff6633, 0xffcc6633, 0xff996633, 0xff666633, 0xff336633, 0xff006633, 0xffff3333, 0xffcc3333, 0xff993333, 0xff663333, 0xff333333, 0xff003333, 0xffff0033,
    0xffcc0033, 0xff990033, 0xff660033, 0xff330033, 0xff000033, 0xffffff00, 0xffccff00, 0xff99ff00, 0xff66ff00, 0xff33ff00, 0xff00ff00, 0xffffcc00, 0xffcccc00, 0xff99cc00, 0xff66cc00, 0xff33cc00,
    0xff00cc00, 0xffff9900, 0xffcc9900, 0xff999900, 0xff669900, 0xff339900, 0xff009900, 0xffff6600, 0xffcc6600, 0xff996600, 0xff666600, 0xff336600, 0xff006600, 0xffff3300, 0xffcc3300, 0xff993300,
    0xff663300, 0xff333300, 0xff003300, 0xffff0000, 0xffcc0000, 0xff990000, 0xff660000, 0xff330000, 0xff0000ee, 0xff0000dd, 0xff0000bb, 0xff0000aa, 0xff000088, 0xff000077, 0xff000055, 0xff000044,
    0xff000022, 0xff000011, 0xff00ee00, 0xff00dd00, 0xff00bb00, 0xff00aa00, 0xff008800, 0xff007700, 0xff005500, 0xff004400, 0xff002200, 0xff001100, 0xffee0000, 0xffdd0000, 0xffbb0000, 0xffaa0000,
    0xff880000, 0xff770000, 0xff550000, 0xff440000, 0xff220000, 0xff110000, 0xffeeeeee, 0xffdddddd, 0xffbbbbbb, 0xffaaaaaa, 0xff888888, 0xff777777, 0xff555555, 0xff444444, 0xff222222, 0xff111111
};

namespace vox_detail {

// Bounds-checked little-endian reader over a byte range.
class reader{
    public:
        reader(const uint8_t* begin, const uint8_t* end) : p(begin), end(end) {}

        bool ok() const { return good; }
        bool at_end() const { return p >= end; }
        size_t left() const { return size_t(end - p); }

        int32_t i32() {
            if (left() < 4) { good = false; p = end; return 0; }
            uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
            p += 4;
            return int32_t(v);
        }

        std::string str() {
            int32_t n = i32();
            if (n < 0 || size_t(n) > left()) { good = false; p = end; return ""; }
            std::string s(reinterpret_cast<const char*>(p), size_t(n));
            p += n;
            return s;
        }

        std::map<std::string, std::string> dict() {
            std::map<std::string, std::string> d;
            int32_t n = i32();
            for (int32_t k = 0; k < n && good; k++) {
                auto key = str();
                d[key] = str();
            }
            return d;
        }

    private:
        const uint8_t* p;
        const uint8_t* end;
        bool good = true;
};

class node{
    public:
        char type = 0;           // 'T', 'G' or 'S'
        bool hidden = false;
        std::vector<int> children;  // Transform: one child; group: many; shape: none
        std::vector<int> models;    // Shape only
        transform xform;            // Transform only
};

// The signed permutation matrix packed in a ROTATION byte: bits 0-1 and 2-3
// are the columns of the non-zero entry in rows 0 and 1 (row 2 takes the
// one left), bits 4, 5, 6 the signs of rows 0, 1, 2.
inline transform rotation_from_byte(int r) {
    int c0 = r & 3, c1 = (r >> 2) & 3;
    int c2 = 3 - c0 - c1;
    double m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    m[0][c0] = (r & 16) ? -1 : 1;
    m[1][c1] = (r & 32) ? -1 : 1;
    m[2][c2] = (r & 64) ? -1 : 1;
    return transform::orthogonal(m);
}

inline int to_int(const std::string& s, int fallback) {
    try { return std::stoi(s); } catch (...) { return fallback; }
}

}  // namespace vox_detail

// Loads path into out; returns false with a message in error on a problem.
inline bool load_vox(const std::string& path, vox_file& out, std::string& error) {
    using namespace vox_detail;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 20 || std::memcmp(bytes.data(), "VOX ", 4) != 0) {
        error = "'" + path + "' is not a MagicaVoxel .vox file";
        return false;
    }

    for (int k = 0; k < 256; k++) {
        uint32_t c = vox_default_palette[k];
        out.palette[k] = color((c & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, ((c >> 16) & 0xff) / 255.0);
    }

    std::map<int, node> nodes;
    const uint8_t* p = bytes.data() + 8;  // After "VOX " and the version
    const uint8_t* end = bytes.data() + bytes.size();
    int sx = 0, sy = 0, sz = 0;

    // MAIN's children are a flat sequence of chunks; walk them in order
    if (end - p < 12 || std::memcmp(p, "MAIN", 4) != 0) {
        error = "'" + path + "' has no MAIN chunk";
        return false;
    }
    {
        reader head(p + 4, p + 12);
        int32_t content = head.i32();
        p += 12 + std::max(content, 0);
    }
    while (end - p >= 12) {
        std::string id(reinterpret_cast<const char*>(p), 4);
        reader head(p + 4, p + 12);
        int32_t content = head.i32(), children = head.i32();
        if (content < 0 || children < 0 || size_t(content) + size_t(children) > size_t(end - p - 12)) {
            error = "'" + path + "' is truncated or corrupt (chunk " + id + ")";
            return false;
        }
        reader in(p + 12, p + 12 + content);
        p += 12 + content + children;

        if (id == "SIZE") {
            sx = in.i32(); sy = in.i32(); sz = in.i32();
            if (sx <= 0 || sy <= 0 || sz <= 0 || sx > 2048 || sy > 2048 || sz > 2048) {
                error = "'" + path + "' has a model of invalid size";
                return false;
            }
        } else if (id == "XYZI") {
            if (sx <= 0) {
                error = "'" + path + "' has voxels before their SIZE";
                return false;
            }
            vox_model m;
            m.nx = sx; m.ny = sy; m.nz = sz;
            m.cells.assign(size_t(sx) * sy * sz, 0);
            int32_t n = in.i32();
            if (n < 0 || size_t(n) * 4 > in.left()) {
                error = "'" + path + "' has a truncated voxel list";
                return false;
            }
            const uint8_t* v = p - children - content + 4;  // The (x, y, z, i) bytes
            for (int32_t k = 0; k < n; k++, v += 4)
                if (v[0] < sx && v[1] < sy && v[2] < sz)
                    m.cells[(size_t(v[2]) * sy + v[1]) * sx + v[0]] = v[3];
            out.models.push_back(std::move(m));
            sx = sy = sz = 0;
        } else if (id == "RGBA") {
            if (content < 1024) {
                error = "'" + path + "' has a short palette";
                return false;
            }
            const uint8_t* c = p - children - content;
            for (int k = 0; k < 255; k++)
                out.palette[k + 1] = color(c[4*k] / 255.0, c[4*k + 1] / 255.0, c[4*k + 2] / 255.0);
        } else if (id == "MATL") {
            int index = in.i32();
            auto props = in.dict();
            if (in.ok() && index >= 0 && index < 256)
                out.materials[index] = props;
        } else if (id == "nTRN") {
            int id_ = in.i32();
            auto attributes = in.dict();
            node& t = nodes[id_];
            t.type = 'T';
            t.hidden = attributes["_hidden"] == "1";
            t.children.push_back(in.i32());
            in.i32();  // Reserved
            in.i32();  // Layer
            int frames = in.i32();
            if (frames > 0) {
                auto frame = in.dict();  // Only the first animation frame is used
                transform rotation;
                if (frame.count("_r"))
                    rotation = rotation_from_byte(to_int(frame["_r"], 4));
                vec3 offset(0, 0, 0);
                if (frame.count("_t")) {
                    std::istringstream xyz(frame["_t"]);
                    xyz >> offset[0] >> offset[1] >> offset[2];
                }
                t.xform = transform::translate(offset).then_after(rotation);
            }
        } else if (id == "nGRP") {
            int id_ = in.i32();
            auto attributes = in.dict();
            node& g = nodes[id_];
            g.type = 'G';
            g.hidden = attributes["_hidden"] == "1";
            int n = in.i32();
            for (int k = 0; k < n && in.ok(); k++)
                g.children.push_back(in.i32());
        } else if (id == "nSHP") {
            int id_ = in.i32();
            in.dict();
            node& s = nodes[id_];
            s.type = 'S';
            int n = in.i32();
            if (n > 0) {
                s.models.push_back(in.i32());  // The first frame's model
                in.dict();
            }
        }
        if (!in.ok()) {
            error = "'" + path + "' has a corrupt " + id + " chunk";
            return false;
        }
    }

    if (out.models.empty()) {
        error = "'" + path + "' has no voxels";
        return false;
    }

    // A cell's model coordinates are relative to the model's center, rounded
    // down, as MagicaVoxel places them
    auto centered = [&](int model) {
        const auto& m = out.models[model];
        return transform::translate(-vec3(m.nx / 2, m.ny / 2, m.nz / 2));
    };

    if (nodes.empty() || !nodes.count(0)) {
        // No scene graph (version 150): each model stands on z = 0, centered in x and y
        for (int k = 0; k < int(out.models.size()); k++)
            out.instances.push_back({k, transform::translate(vec3(0, 0, out.models[k].nz / 2)).then_after(centered(k))});
        return true;
    }

    // Walk the graph from the root, composing transforms; depth-limited so a
    // cyclic file can't recurse forever
    std::vector<std::pair<int, transform>> stack = {{0, transform()}};
    std::vector<int> depth = {0};
    while (!stack.empty()) {
        auto [id, xform] = stack.back();
        int d = depth.back();
        stack.pop_back();
        depth.pop_back();
        auto it = nodes.find(id);
        if (it == nodes.end() || it->second.hidden || d > 64)
            continue;
        const node& n = it->second;
        transform here = n.type == 'T' ? xform.then_after(n.xform) : xform;
        for (int child : n.children) {
            stack.push_back({child, here});
            depth.push_back(d + 1);
        }
        for (int model : n.models)
            if (model >= 0 && model < int(out.models.size()))
                out.instances.push_back({model, here.then_after(centered(model))});
    }
    return true;
}

#endif
