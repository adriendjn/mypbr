#ifndef BVH_H
#define BVH_H

#include "aabb.h"
#include "hittable.h"
#include "hittable_list.h"

#include <algorithm>
#include <memory>
#include <vector>

// Bounding volume hierarchy over a set of hittables.
//
// Built top-down with the surface area heuristic (binned on primitive
// centroids), then stored flattened in depth-first order: a node's left
// child is the next node in the array, and interior nodes record where their
// right child starts. Traversal is iterative with a small stack and visits
// the nearer child first, so far subtrees are usually culled by the closest
// hit found so far.
class bvh : public hittable{
    public:
        explicit bvh(const hittable_list& list) : bvh(list.objects) {}

        explicit bvh(const std::vector<std::shared_ptr<hittable>>& objects) : owned(objects) {
            if (owned.empty()) {
                nodes.push_back(node());
                return;
            }

            std::vector<build_item> items;
            items.reserve(owned.size());
            for (size_t k = 0; k < owned.size(); k++) {
                auto box = owned[k]->bounding_box();
                items.push_back({box, box.centroid(), owned[k].get()});
            }

            nodes.reserve(2 * items.size());
            build(items, 0, items.size());

            primitives.reserve(items.size());
            for (const auto& item : items)
                primitives.push_back(item.object);
        }

        bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
            const vec3& d = r.direction();
            vec3 inv_dir(1/d.x(), 1/d.y(), 1/d.z());
            bool dir_is_neg[3] = { inv_dir.x() < 0, inv_dir.y() < 0, inv_dir.z() < 0 };

            bool hit_anything = false;
            int stack[128];
            int stack_size = 0;
            int current = 0;

            while (true) {
                const node& n = nodes[current];
                if (n.box.hit(r.origin(), inv_dir, ray_t)) {
                    if (n.count > 0) {
                        // Leaf: test its primitives, shrinking the interval on every hit
                        for (int k = 0; k < n.count; k++) {
                            if (primitives[n.offset + k]->hit(r, ray_t, rec)) {
                                hit_anything = true;
                                ray_t.max = rec.t;
                            }
                        }
                        if (stack_size == 0) break;
                        current = stack[--stack_size];
                    } else if (dir_is_neg[n.axis]) {
                        // Ray goes towards -axis: the right (upper) child is nearer
                        stack[stack_size++] = current + 1;
                        current = n.offset;
                    } else {
                        stack[stack_size++] = n.offset;
                        current = current + 1;
                    }
                } else {
                    if (stack_size == 0) break;
                    current = stack[--stack_size];
                }
            }

            return hit_anything;
        }

        aabb bounding_box() const override { return nodes[0].box; }

        size_t node_count() const { return nodes.size(); }

    private:
        struct node{
            aabb box;
            int offset = 0;  // Leaf: first primitive. Interior: index of the right child.
            int count  = 0;  // Number of primitives; 0 for interior nodes.
            int axis   = 0;  // Interior: the axis the children were split along.
        };

        struct build_item{
            aabb box;
            point3 centroid;
            const hittable* object;
        };

        std::vector<std::shared_ptr<hittable>> owned;  // Keeps the objects alive
        std::vector<const hittable*> primitives;       // In leaf order
        std::vector<node> nodes;

        static constexpr int bin_count = 16;
        static constexpr int max_leaf_size = 4;

        // Builds the subtree over items[begin, end) and returns its node index.
        int build(std::vector<build_item>& items, size_t begin, size_t end) {
            int index = int(nodes.size());
            nodes.push_back(node());

            aabb bounds, centroid_bounds;
            for (size_t k = begin; k < end; k++) {
                bounds = aabb(bounds, items[k].box);
                centroid_bounds = aabb(centroid_bounds, aabb(items[k].centroid, items[k].centroid));
            }
            nodes[index].box = bounds;

            size_t count = end - begin;
            int axis = centroid_bounds.longest_axis();
            const interval& extent = centroid_bounds.axis_interval(axis);

            if (count == 1 || extent.size() <= 0)
                return make_leaf(index, begin, count, items, axis);

            // Bin the centroids along the axis and evaluate the SAH cost of
            // splitting after each bin: cost ~ A_left * N_left + A_right * N_right
            struct bin{ aabb box; int count = 0; };
            bin bins[bin_count];
            auto bin_of = [&](const point3& c) {
                int b = int(bin_count * (c[axis] - extent.min) / extent.size());
                return std::min(b, bin_count - 1);
            };
            for (size_t k = begin; k < end; k++) {
                auto& b = bins[bin_of(items[k].centroid)];
                b.box = aabb(b.box, items[k].box);
                b.count++;
            }

            double best_cost = infinity;
            int best_split = -1;
            for (int split = 0; split < bin_count - 1; split++) {
                aabb left, right;
                int left_count = 0, right_count = 0;
                for (int b = 0; b <= split; b++) {
                    left = aabb(left, bins[b].box);
                    left_count += bins[b].count;
                }
                for (int b = split + 1; b < bin_count; b++) {
                    right = aabb(right, bins[b].box);
                    right_count += bins[b].count;
                }
                if (left_count == 0 || right_count == 0)
                    continue;
                double cost = left.surface_area() * left_count + right.surface_area() * right_count;
                if (cost < best_cost) {
                    best_cost = cost;
                    best_split = split;
                }
            }

            // A leaf costs one intersection per primitive; an interior node
            // adds a traversal step (relative cost ~1/8 of an intersection).
            double leaf_cost = double(count);
            double split_cost = 0.125 + best_cost / bounds.surface_area();
            if (best_split < 0 || (count <= max_leaf_size && leaf_cost <= split_cost))
                return make_leaf(index, begin, count, items, axis);

            auto mid_it = std::partition(items.begin() + begin, items.begin() + end,
                [&](const build_item& item) { return bin_of(item.centroid) <= best_split; });
            size_t mid = size_t(mid_it - items.begin());

            nodes[index].axis = axis;
            build(items, begin, mid);  // Left child lands at index + 1
            int right = build(items, mid, end);
            nodes[index].offset = right;
            return index;
        }

        int make_leaf(int index, size_t begin, size_t count, std::vector<build_item>& items, int axis) {
            // Too many primitives with identical centroids can't be separated
            // by centroid; split them in the middle of the list instead.
            if (count > size_t(max_leaf_size)) {
                size_t mid = begin + count / 2;
                nodes[index].axis = axis;
                build(items, begin, mid);
                int right = build(items, mid, begin + count);
                nodes[index].offset = right;
                return index;
            }
            nodes[index].offset = int(begin);
            nodes[index].count = int(count);
            return index;
        }
};

#endif
