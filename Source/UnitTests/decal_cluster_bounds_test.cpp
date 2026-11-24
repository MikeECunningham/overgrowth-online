#include <Graphics/decal_cluster_bounds.h>
#include <tut/tut.hpp>
#include <limits>

namespace tut {
struct decal_cluster_bounds_data {
    Box box;
    mat4 world, view, projection;
    DecalClusterBounds::Bounds bounds;
    DecalClusterBounds::Grid grid;
    decal_cluster_bounds_data() {
        box.center = vec3(0.0f); box.dims = vec3(2.0f);
        world.SetTranslation(vec3(0, 0, -4));
        // Perspective, with x/y already mapped into an 8x8 cluster grid.
        projection = mat4(0.0f);
        projection.entries[0] = projection.entries[5] = 4;
        projection.entries[8] = projection.entries[9] = -4;
        projection.entries[10] = projection.entries[11] = -1;
        projection.entries[14] = -0.2f;
    }
    DecalClusterBounds::Result Project() {
        return DecalClusterBounds::Project(box, world, view, projection, 0.1f, bounds);
    }
    DecalClusterBounds::Result Clip() {
        return DecalClusterBounds::ClipToGrid(bounds, vec3(0, 0, -100), vec3(8, 8, 0),
                                              8, 8, 16, 0.1f, 16.0f / std::log(100.9f), grid);
    }
};
typedef test_group<decal_cluster_bounds_data> decal_cluster_bounds_group;
decal_cluster_bounds_group decal_cluster_bounds_tests("DecalClusterBounds");
typedef decal_cluster_bounds_group::object decal_cluster_bounds_test;

template<> template<> void decal_cluster_bounds_test::test<1>() {
    using namespace DecalClusterBounds;
    set_test_name("valid perspective bounds retain their volume and grid cells");
    ensure("finite box projects", Project() == Result::Visible);
    ensure_distance(bounds.minimum[0], 4.0f - 4.0f / 3.0f, 0.00001f);
    ensure_distance(bounds.maximum[0], 4.0f + 4.0f / 3.0f, 0.00001f);
    ensure_equals(bounds.minimum[2], -5.0f);
    ensure_equals(bounds.maximum[2], -3.0f);
    ensure("grid range valid", Clip() == Result::Visible);
    ensure_equals(grid.x_min, 2u); ensure_equals(grid.x_max, 6u);
    ensure_equals(grid.y_min, 2u); ensure_equals(grid.y_max, 6u);
    ensure("depth range ordered and bounded", grid.z_min < grid.z_max && grid.z_max < 16);
}
template<> template<> void decal_cluster_bounds_test::test<2>() {
    using namespace DecalClusterBounds;
    set_test_name("near-plane crossings clip while behind/offscreen/far decals cull");
    world.SetTranslation(vec3(0, 0, -0.5f));
    ensure("near crossing projects", Project() == Result::Visible);
    ensure_equals(bounds.maximum[2], -0.1f);
    ensure("near crossing clips to screen", Clip() == Result::Visible);
    ensure_equals(grid.x_min, 0u); ensure_equals(grid.x_max, 8u);
    world.SetTranslation(vec3(0, 0, 4));
    ensure("behind the camera", Project() == Result::Culled);
    world.SetTranslation(vec3(100, 0, -4));
    ensure("offscreen finite projection", Project() == Result::Visible);
    ensure("offscreen culling", Clip() == Result::Culled);
    world.SetTranslation(vec3(0, 0, -102));
    ensure("past far plane projects", Project() == Result::Visible);
    ensure("far culling", Clip() == Result::Culled);
}
template<> template<> void decal_cluster_bounds_test::test<3>() {
    using namespace DecalClusterBounds;
    set_test_name("hidden/flat/singular decals skip; mirrored nonzero decals survive");
    world.SetScale(vec3(0));
    ensure("zero-scale hidden shadow", Project() == Result::Degenerate);
    world.LoadIdentity(); world.SetTranslation(vec3(0, 0, -4));
    world.entries[4] = 1; world.entries[5] = 0;
    ensure("collinear basis", Project() == Result::Degenerate);
    world.LoadIdentity(); world.SetTranslation(vec3(0, 0, -4));
    box.dims[1] = 0;
    ensure("flat local box", Project() == Result::Degenerate);
    box.dims = vec3(2); world.entries[0] = -1;
    ensure("mirrored scale remains supported", Project() == Result::Visible);
}
template<> template<> void decal_cluster_bounds_test::test<4>() {
    using namespace DecalClusterBounds;
    set_test_name("nonfinite source and view transforms never reach projection");
    world.entries[12] = std::numeric_limits<float>::quiet_NaN();
    ensure("NaN world", Project() == Result::InvalidWorld);
    world.SetTranslation(vec3(0, 0, -4));
    world.entries[0] = std::numeric_limits<float>::infinity();
    ensure("infinite scale", Project() == Result::InvalidWorld);
    world.LoadIdentity(); world.SetTranslation(vec3(0, 0, -4));
    view.entries[0] = std::numeric_limits<float>::quiet_NaN();
    ensure("NaN view", Project() == Result::InvalidView);
    view.LoadIdentity(); view.entries[0] = std::numeric_limits<float>::max();
    world.entries[0] = 2;
    ensure("finite inputs that overflow in view space", Project() == Result::InvalidView);
}
template<> template<> void decal_cluster_bounds_test::test<5>() {
    using namespace DecalClusterBounds;
    set_test_name("NaN, zero, negative and tiny homogeneous w reject before division");
    projection.entries[11] = 0;
    ensure("zero w", Project() == Result::InvalidProjection);
    projection.entries[11] = 1;
    ensure("negative w", Project() == Result::InvalidProjection);
    projection.entries[11] = -0.000000001f;
    ensure("near-zero w", Project() == Result::InvalidProjection);
    projection.entries[11] = -1;
    projection.entries[0] = std::numeric_limits<float>::quiet_NaN();
    ensure("NaN projected x despite finite w", Project() == Result::InvalidProjection);
}
template<> template<> void decal_cluster_bounds_test::test<6>() {
    using namespace DecalClusterBounds;
    set_test_name("invalid grid data cannot become unsigned indices");
    ensure("initial valid box", Project() == Result::Visible);
    bounds.minimum[0] = std::numeric_limits<float>::quiet_NaN();
    ensure("NaN index input", Clip() == Result::InvalidGrid);
    Project(); bounds.minimum[0] = bounds.maximum[0] + 1;
    ensure("reversed bounds", Clip() == Result::InvalidGrid);
    Project();
    ensure("infinite log multiplier", ClipToGrid(bounds, vec3(0, 0, -100), vec3(8, 8, 0), 8, 8, 16,
                                                   0.1f, std::numeric_limits<float>::infinity(), grid) == Result::InvalidGrid);
    ensure("zero grid", ClipToGrid(bounds, vec3(0, 0, -100), vec3(8, 8, 0), 0, 8, 16, 0.1f, 1, grid) == Result::InvalidGrid);
    Project();
    ensure("split view clips to its own cells", ClipToGrid(bounds, vec3(0, 0, -100), vec3(4, 8, 0),
                                                            8, 8, 16, 0.1f, 16.0f / std::log(100.9f), grid) == Result::Visible);
    ensure_equals(grid.x_max, 4u);
    Project();
    bounds.maximum[0] = float(std::numeric_limits<unsigned>::max());
    ensure("rounded UINT_MAX cannot overflow unsigned conversion",
           ClipToGrid(bounds, vec3(0, 0, -100), vec3(bounds.maximum[0], 8, 0),
                      std::numeric_limits<unsigned>::max(), 8, 16, 0.1f, 1, grid) == Result::InvalidGrid);
}
template<> template<> void decal_cluster_bounds_test::test<7>() {
    using namespace DecalClusterBounds;
    set_test_name("camera inversion is checked before calling float inverse");
    ensure("perspective invertible", Invertible(projection));
    ensure("view invertible", Invertible(view));
    mat4 singular(0.0f);
    ensure("zero matrix singular", !Invertible(singular));
    singular.LoadIdentity(); singular.entries[4] = 1; singular.entries[5] = 0;
    ensure("duplicate columns singular", !Invertible(singular));
    singular.LoadIdentity(); singular.entries[10] = std::numeric_limits<float>::infinity();
    ensure("infinite matrix rejected", !Invertible(singular));
}
}  // namespace tut
