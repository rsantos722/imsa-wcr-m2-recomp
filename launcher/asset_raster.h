// Depth-tested, SDL-free rendering for the launcher's offline model preview.
// SDL's 2D geometry API has no depth buffer or perspective-correct UVs.
#pragma once

#include "asset_viewport.h"
#include <cstddef>

namespace launcher_assets {

struct PreviewInstance {
    const Part* part = nullptr;
    Transform transform{};
};

struct PreviewVertex {
    Vec3 view; // camera coordinates; positive Z is distance from the eye
    Vec2 uv;
    float shade = 1.0f;
};

class PreviewRaster {
public:
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::vector<float> depth; // reciprocal distance, zero means background

    void reset(int w, int h);
    // Clips against the near plane before projection. Alpha-zero texels never
    // occlude; opaque texels write depth, partial alpha blends in a second pass.
    void triangle(const Camera& camera, const PreviewVertex (&vertices)[3],
                  const TextureImage* texture, bool translucent = false,
                  bool wireframe = false);
};

void render_preview(PreviewRaster& image, const Camera& camera,
                    const PreviewInstance* instances, size_t count,
                    float yaw, float pitch, bool lighting, bool wireframe);

} // namespace launcher_assets
