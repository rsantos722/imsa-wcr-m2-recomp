// Camera and projection math for the Asset Viewer's 3D panel.
//
// SDL-free and ImGui-free: main.cpp turns the projected points into
// a depth-tested preview image, and `m2_launcher_assets` checks
// that the fitted camera actually keeps a decoded model inside the panel.
#pragma once

#include "asset_model.h"

#include <algorithm>
#include <cmath>

namespace launcher_assets {

inline int wheel_placements(const Model& model, int body,
                            std::array<Transform, 4>& out) {
    if (model.race_pack) {
        // Original ELF .data+0x5d98, wheelpos[body][wheel]. GetCarPosition
        // @0x1402c..0x140a4 adjusts the simulation's wheel matrices by this
        // authored table minus the physics mounts. Physics axle coordinates
        // alone therefore put the preview wheels in the wrong places.
        constexpr Vec3 mounts[4][4] = {
            {{-2.59f,0,-3.72f},{2.55f,0,-3.72f},{2.55f,0,3.78f},{-2.59f,0,3.78f}},
            {{-2.54f,0,-4.06f},{2.61f,0,-4.06f},{2.61f,0,4.27f},{-2.54f,0,4.27f}},
            {{-2.51f,0,-3.5f},{2.51f,0,-3.5f},{2.51f,0,4.36f},{-2.51f,0,4.36f}},
            {{-2.59f,0,-4.06f},{2.55f,0,-4.06f},{2.55f,0,4.28f},{-2.59f,0,4.28f}}};
        if (body < 0 || body >= 4) return 0;
        for (int i = 0; i < 4; ++i) {
            out[i] = Transform{};
            out[i].m[9] = mounts[body][i].x;
            out[i].m[10] = mounts[body][i].y;
            out[i].m[11] = mounts[body][i].z;
        }
    } else {
        if (model.wheel_count != 4) return 0;
        out = model.wheel_transforms;
    }
    // MS_addcar@0x23e8c/0x23e98 and MS_addcar_showroom@0x2401c/0x24028
    // negate the first two basis rows on wheels 0 and 3: turn the rim outwards.
    for (const int i : {0, 3})
        for (int j = 0; j < 6; ++j) out[i].m[j] = -out[i].m[j];
    return 4;
}

// Model space is +Y up. The rotation orbits the model: `yaw` turns about Y and
// `pitch` tilts about the view's X axis.
inline Vec3 rotate_view(const Vec3& v, float yaw, float pitch) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float x1 = cy * v.x + sy * v.z;
    const float z1 = -sy * v.x + cy * v.z;
    return {x1, cp * v.y - sp * z1, sp * v.y + cp * z1};
}

struct Camera {
    Vec3 center{};
    float radius = 1.0f;
    float focal = 1.0f;     // pixels
    float distance = 1.0f;  // from the model centre along the view axis
    float cx = 0.0f, cy = 0.0f;  // panel centre in points
};

// Frame the box so its bounding sphere fills ~85% of the smaller panel edge at
// zoom 1. A larger zoom shortens the camera distance rather than changing the
// focal length, so the perspective stays fixed.
inline Camera fit_camera(const Vec3& lo, const Vec3& hi, float panel_width,
                         float panel_height, float zoom) {
    Camera camera;
    camera.center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                     (lo.z + hi.z) * 0.5f};
    const float dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
    camera.radius = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);
    camera.cx = panel_width * 0.5f;
    camera.cy = panel_height * 0.5f;
    camera.focal = 2.2f * std::max(0.5f * std::min(panel_width, panel_height), 1.0f);
    camera.distance = 2.6f * camera.radius / std::clamp(zoom, 0.05f, 100.0f);
    return camera;
}

// The camera sits on -Z and looks toward +Z with +Y up. Its screen-right
// basis is therefore -X (forward cross up), not +X. Using +X mirrors every
// livery when viewed from the outside. `depth` is the view-space distance.
inline void project(const Camera& camera, const Vec3& point, float yaw, float pitch,
                    float& x, float& y, float& depth) {
    Vec3 p = {point.x - camera.center.x, point.y - camera.center.y,
              point.z - camera.center.z};
    p = rotate_view(p, yaw, pitch);
    depth = p.z + camera.distance;
    const float safe = std::max(depth, 1e-4f);
    x = camera.cx - camera.focal * p.x / safe;
    y = camera.cy - camera.focal * p.y / safe;
}

inline Vec3 light_direction() { return {0.34f, 0.52f, -0.78f}; }

}  // namespace launcher_assets
