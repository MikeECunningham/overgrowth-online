// Finite decal/light bounds before homogeneous division and grid indexing.
// Apache-2.0.
#pragma once

#include <Math/mat4.h>
#include <Math/overgrowth_geometry.h>
#include <Math/vec3math.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace DecalClusterBounds {
constexpr float kMinimumW = 0.000001f;
constexpr float kMinimumAxis = 0.00000001f;

enum class Result { Visible, Culled, Degenerate, InvalidWorld, InvalidView, InvalidProjection, InvalidGrid };

inline bool Finite(const vec3& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
inline bool Finite(const vec4& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]) && std::isfinite(value[3]);
}
inline bool Finite(const mat4& value) {
    for (float entry : value.entries) if (!std::isfinite(entry)) return false;
    return true;
}

// Check singular camera matrices before asking the engine's float inverse to
// divide by their determinant. Double intermediates also avoid overflowing the
// determinant merely because a finite world has large coordinates.
inline bool Invertible(const mat4& matrix) {
    if (!Finite(matrix)) return false;
    double rows[4][4];
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) rows[row][column] = matrix.entries[column * 4 + row];
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column])) pivot = row;
        if (rows[pivot][column] == 0.0) return false;
        if (pivot != column) for (int j = column; j < 4; ++j) std::swap(rows[pivot][j], rows[column][j]);
        for (int row = column + 1; row < 4; ++row) {
            const double factor = rows[row][column] / rows[column][column];
            for (int j = column + 1; j < 4; ++j) rows[row][j] -= factor * rows[column][j];
        }
    }
    return true;
}

inline bool HasVolume(const mat4& world) {
    double lengths[3] = {};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            const double entry = world.entries[column * 4 + row];
            lengths[column] += entry * entry;
        }
        if (lengths[column] <= double(kMinimumAxis) * kMinimumAxis) return false;
    }
    const double a = world.entries[0], b = world.entries[4], c = world.entries[8];
    const double d = world.entries[1], e = world.entries[5], f = world.entries[9];
    const double g = world.entries[2], h = world.entries[6], i = world.entries[10];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    return std::abs(determinant) > 0.00000001 * std::sqrt(lengths[0] * lengths[1] * lengths[2]);
}

inline vec3 Corner(const Box& box, int index) {
    // Same eight corners as Box::GetPoint; order does not affect the bounds.
    return vec3(box.center[0] + box.dims[0] * (index & 1 ? 0.5f : -0.5f),
                box.center[1] + box.dims[1] * (index & 2 ? 0.5f : -0.5f),
                box.center[2] + box.dims[2] * (index & 4 ? 0.5f : -0.5f));
}

struct Bounds { vec3 minimum, maximum; };
struct Grid { unsigned x_min, x_max, y_min, y_max, z_min, z_max; };

inline Result ProjectViewBounds(vec3 view_min, vec3 view_max, const mat4& projection, float near_plane, Bounds& bounds) {
    if (!Finite(view_min) || !Finite(view_max)) return Result::InvalidView;
    if (!Finite(projection) || !std::isfinite(near_plane) || near_plane <= kMinimumW) return Result::InvalidProjection;
    for (int axis = 0; axis < 3; ++axis) if (view_min[axis] > view_max[axis]) return Result::InvalidView;
    if (view_min[2] > -near_plane) return Result::Culled;
    view_max[2] = std::min(-near_plane, view_max[2]);
    Box clipped;
    clipped.center = (view_min + view_max) * 0.5f;
    clipped.dims = view_max - view_min;
    if (!Finite(clipped.center) || !Finite(clipped.dims)) return Result::InvalidView;

    vec3 minimum(std::numeric_limits<float>::max()), maximum(-std::numeric_limits<float>::max());
    for (int point = 0; point < Box::NUM_POINTS; ++point) {
        const vec3 corner = Corner(clipped, point);
        if (!Finite(corner)) return Result::InvalidView;
        const vec4 projected = projection * vec4(corner, 1.0f);
        // NaN fails this positive test, but check all components so a bad x/y
        // cannot sneak through the old w-only assertion into an integer cast.
        if (!Finite(projected) || projected[3] <= kMinimumW) return Result::InvalidProjection;
        const float x = projected[0] / projected[3], y = projected[1] / projected[3];
        if (!std::isfinite(x) || !std::isfinite(y)) return Result::InvalidProjection;
        minimum[0] = std::min(minimum[0], x); minimum[1] = std::min(minimum[1], y);
        maximum[0] = std::max(maximum[0], x); maximum[1] = std::max(maximum[1], y);
    }
    minimum[2] = view_min[2]; maximum[2] = view_max[2];
    bounds.minimum = minimum; bounds.maximum = maximum;
    return Result::Visible;
}

inline Result Project(const Box& box, const mat4& world, const mat4& view, const mat4& projection,
                      float near_plane, Bounds& bounds) {
    if (!Finite(world) || !Finite(box.center) || !Finite(box.dims)) return Result::InvalidWorld;
    for (int axis = 0; axis < 3; ++axis) {
        if (box.dims[axis] < 0.0f) return Result::InvalidWorld;
        if (box.dims[axis] == 0.0f) return Result::Degenerate;
    }
    // A zero scale is the existing convention for a hidden character shadow.
    // Skip it as empty geometry rather than projecting or inverting it.
    if (!HasVolume(world)) return Result::Degenerate;
    if (!Finite(view)) return Result::InvalidView;
    const mat4 model_view = view * world;
    if (!Finite(model_view)) return Result::InvalidView;
    vec3 minimum(std::numeric_limits<float>::max()), maximum(-std::numeric_limits<float>::max());
    for (int point = 0; point < Box::NUM_POINTS; ++point) {
        const vec3 corner = Corner(box, point);
        if (!Finite(corner)) return Result::InvalidWorld;
        const vec4 transformed = model_view * vec4(corner, 1.0f);
        if (!Finite(transformed)) return Result::InvalidView;
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], transformed[axis]);
            maximum[axis] = std::max(maximum[axis], transformed[axis]);
        }
    }
    return ProjectViewBounds(minimum, maximum, projection, near_plane, bounds);
}

inline Result ClipToGrid(Bounds& bounds, const vec3& lower, const vec3& upper,
                         unsigned width, unsigned height, unsigned depth,
                         float near_plane, float z_multiplier, Grid& grid) {
    if (!Finite(bounds.minimum) || !Finite(bounds.maximum) || !Finite(lower) || !Finite(upper) ||
        !width || !height || !depth || !std::isfinite(near_plane) || near_plane <= kMinimumW ||
        !std::isfinite(z_multiplier) || z_multiplier <= 0.0f) return Result::InvalidGrid;
    for (int axis = 0; axis < 3; ++axis)
        if (bounds.minimum[axis] > bounds.maximum[axis] || lower[axis] > upper[axis]) return Result::InvalidGrid;
    if (lower[0] < 0.0f || lower[1] < 0.0f || upper[0] > width || upper[1] > height) return Result::InvalidGrid;
    for (int axis = 0; axis < 3; ++axis) {
        if (bounds.minimum[axis] > upper[axis] || bounds.maximum[axis] < lower[axis]) return Result::Culled;
        bounds.minimum[axis] = std::max(lower[axis], std::min(upper[axis], bounds.minimum[axis]));
        bounds.maximum[axis] = std::max(lower[axis], std::min(upper[axis], bounds.maximum[axis]));
        if (bounds.minimum[axis] >= bounds.maximum[axis]) return Result::Culled;
    }
    const float near_argument = -bounds.maximum[2] - near_plane + 1.0f;
    const float far_argument = -bounds.minimum[2] - near_plane + 1.0f;
    if (near_argument <= 0.0f || far_argument <= 0.0f) return Result::InvalidGrid;
    const double z_min = std::log(near_argument) * z_multiplier;
    const double z_max = std::ceil(std::log(far_argument) * z_multiplier);
    // Validate in floating point before ANY float-to-unsigned conversion.
    if (!std::isfinite(z_min) || !std::isfinite(z_max) || z_min < 0 || z_max < 0 ||
        z_min > depth || z_max > double(depth) + 1.0) return Result::InvalidGrid;
    const double x_max = std::ceil(double(bounds.maximum[0])), y_max = std::ceil(double(bounds.maximum[1]));
    const double integer_max = std::numeric_limits<unsigned>::max();
    if (double(bounds.minimum[0]) > width || double(bounds.minimum[1]) > height ||
        x_max > width || y_max > height || x_max > integer_max || y_max > integer_max) return Result::InvalidGrid;
    grid.x_min = static_cast<unsigned>(bounds.minimum[0]);
    grid.x_max = static_cast<unsigned>(x_max);
    grid.y_min = static_cast<unsigned>(bounds.minimum[1]);
    grid.y_max = static_cast<unsigned>(y_max);
    grid.z_min = static_cast<unsigned>(z_min);
    grid.z_max = static_cast<unsigned>(std::min(z_max, double(depth - 1)));
    if (grid.x_max > width || grid.y_max > height) return Result::InvalidGrid;
    if (grid.x_min >= grid.x_max || grid.y_min >= grid.y_max || grid.z_min >= grid.z_max) return Result::Culled;
    return Result::Visible;
}
}  // namespace DecalClusterBounds
