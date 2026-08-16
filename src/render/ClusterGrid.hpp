#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace ege {

    // Clustered shading: dicing the view frustum so a fragment only pays for
    // the lights that reach it.
    //
    // The forward shader this replaces looped every light for every fragment.
    // At sixteen lights that is merely wasteful; at four hundred it is the
    // frame. Clustering splits the frustum into a grid of cells - froxels,
    // since they are frustum-shaped rather than cubes - assigns each light to
    // the cells its volume touches, and lets a fragment look up its own cell
    // and loop only what is in it. The cost stops scaling with the number of
    // lights in the scene and starts scaling with the number that actually
    // overlap the pixel.
    //
    // Everything in this header is arithmetic on a projection matrix with no
    // Vulkan in sight, which is what lets the grid be unit-tested on a machine
    // with no GPU. The light-culling compute shader implements the same
    // definitions - `clusterBounds` and `sphereTouchesCluster` are the
    // specification it is written against, and the tests are what pin them.

    // 16 x 9 tiles across the screen, 24 slices deep. The tile counts follow
    // the usual 16:9 framing so cells stay roughly square; the depth count is
    // the number at which the exponential slicing below stops visibly
    // clumping lights near the camera.
    inline constexpr uint32_t clusterGridX = 16;
    inline constexpr uint32_t clusterGridY = 9;
    inline constexpr uint32_t clusterGridZ = 24;
    inline constexpr uint32_t clusterCount = clusterGridX * clusterGridY * clusterGridZ;

    // How many lights one cluster records. A cell that overflows drops the
    // excess rather than growing, because a fixed stride is what makes the
    // index list addressable without a per-cluster offset table. The renderer
    // counts overflows so the limit is visible rather than silent.
    inline constexpr uint32_t maxLightsPerCluster = 64;

    // How many lights the scene buffer holds. Unlike the sixteen this
    // replaces, it is a bound on storage rather than on shader work: a
    // fragment never loops over it, only over its own cluster's list.
    inline constexpr uint32_t maxSceneLights = 1024;

    // The frustum being diced. Near and far are the clustering range, which is
    // the camera's near plane and however far lights are worth culling to -
    // not necessarily the camera's far plane.
    struct ClusterGrid {
        uint32_t x = clusterGridX;
        uint32_t y = clusterGridY;
        uint32_t z = clusterGridZ;
        float nearPlane = 0.1f;
        float farPlane = 100.f;
    };

    // A cluster's bounds in view space, where the camera sits at the origin.
    //
    // Which way "forward" points is the projection's business, not this
    // header's: bounds come back in whatever handedness the matrix passed in
    // uses. This engine's camera looks down +Z; GLM's own constructors look
    // down -Z. A "view depth" below always means a positive distance along
    // the forward axis whichever it is, which is the measure a shader has to
    // reproduce - and getting that sign wrong is invisible in a small scene
    // and wrong everywhere in a large one.
    struct ClusterBounds {
        glm::vec3 minPoint{0.f};
        glm::vec3 maxPoint{0.f};
    };

    // Where slice `slice` begins, as a positive distance along the view's
    // forward axis. Slice 0 begins at the near plane and slice `z` ends at the
    // far plane, so this is valid for slice in [0, z].
    //
    // The spacing is exponential - z_k = near * (far/near)^(k/z) - because
    // perspective compresses distance the same way. Uniform slices would put
    // almost every cell in the far half of the frustum, where a cell spans
    // acres and every light in the distance lands in the same one.
    float clusterSliceDepth(const ClusterGrid& grid, uint32_t slice);

    // Which slice a view depth falls in, clamped to the grid. The inverse of
    // clusterSliceDepth, rounded down.
    uint32_t clusterSliceForDepth(const ClusterGrid& grid, float viewDepth);

    // The same inverse in the form a shader wants it: slice =
    // floor(log2(depth) * scale + bias), which is two instructions instead of
    // a division and two logarithms. x is the scale, y the bias.
    glm::vec2 clusterSliceScaleBias(const ClusterGrid& grid);

    // The view-space bounding box of one cluster.
    //
    // `inverseProjection` unprojects clip space back to view space, so this
    // works for any projection the camera can hold rather than re-deriving a
    // frustum from a field of view. The box is axis-aligned and therefore
    // slightly larger than the froxel it bounds - a light can be assigned to a
    // cell it does not quite reach, which costs a wasted iteration and never a
    // missing light.
    ClusterBounds clusterBounds(
        const ClusterGrid& grid,
        const glm::mat4& inverseProjection,
        uint32_t ix,
        uint32_t iy,
        uint32_t iz);

    // Whether a sphere overlaps a cluster, both in view space. The standard
    // clamp-to-box test: the closest point in the box to the centre, compared
    // against the radius.
    bool sphereTouchesCluster(const ClusterBounds& bounds, glm::vec3 centre, float radius);

    // Flattens a cluster coordinate into the index the light list is keyed by.
    // X varies fastest so that the tiles of one depth slice are contiguous,
    // which is the order a screen-space walk touches them in.
    inline uint32_t clusterIndex(const ClusterGrid& grid, uint32_t ix, uint32_t iy, uint32_t iz) {
        return ix + grid.x * (iy + grid.y * iz);
    }

}  // namespace ege
