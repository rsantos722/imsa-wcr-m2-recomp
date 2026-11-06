#include "asset_raster.h"

#include <array>

namespace launcher_assets {

void PreviewRaster::reset(int w, int h) {
    width = std::clamp(w, 1, 2048);
    height = std::clamp(h, 1, 2048);
    rgba.resize(size_t(width) * height * 4);
    depth.assign(size_t(width) * height, 0.0f);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = 14; rgba[i+1] = 16; rgba[i+2] = 22; rgba[i+3] = 255;
    }
}

namespace {
struct ScreenVertex { float x, y, q, u, v, shade; };
// Subpixel edge equations give adjacent faces exactly the same shared edge,
// and need only three integer additions per pixel while stepping a row.
struct RasterPoint { int64_t x, y; };
int64_t edge(const RasterPoint& a, const RasterPoint& b, int64_t x, int64_t y) {
    return (b.x-a.x)*(y-a.y) - (b.y-a.y)*(x-a.x);
}
bool top_left(const RasterPoint& a, const RasterPoint& b) {
    return b.y < a.y || (b.y == a.y && b.x > a.x);
}
PreviewVertex interpolate(const PreviewVertex& a, const PreviewVertex& b, float t) {
    return {{a.view.x + (b.view.x-a.view.x)*t,
             a.view.y + (b.view.y-a.view.y)*t,
             a.view.z + (b.view.z-a.view.z)*t},
            {a.uv.u + (b.uv.u-a.uv.u)*t, a.uv.v + (b.uv.v-a.uv.v)*t},
            a.shade + (b.shade-a.shade)*t};
}
// Inverse-transpose of the instance basis, including nonuniform axle scale.
Vec3 normal(const Transform& t, const Vec3& n) {
    const auto& m = t.m;
    const Vec3 a{m[4]*m[8]-m[5]*m[7], m[5]*m[6]-m[3]*m[8], m[3]*m[7]-m[4]*m[6]};
    const Vec3 b{m[7]*m[2]-m[8]*m[1], m[8]*m[0]-m[6]*m[2], m[6]*m[1]-m[7]*m[0]};
    const Vec3 c{m[1]*m[5]-m[2]*m[4], m[2]*m[3]-m[0]*m[5], m[0]*m[4]-m[1]*m[3]};
    const float det = m[0]*a.x + m[1]*a.y + m[2]*a.z;
    const float sign = det < 0 ? -1.0f : 1.0f;
    return {sign*(a.x*n.x+b.x*n.y+c.x*n.z),
            sign*(a.y*n.x+b.y*n.y+c.y*n.z),
            sign*(a.z*n.x+b.z*n.y+c.z*n.z)};
}
} // namespace

void PreviewRaster::triangle(const Camera& camera, const PreviewVertex (&vertices)[3],
                             const TextureImage* texture, bool translucent,
                             bool wireframe) {
    if (width <= 0 || height <= 0) return;
    const float near = std::max(camera.radius * 0.01f, 1e-4f);
    // One-plane Sutherland-Hodgman: a crossing triangle becomes a quad, rather
    // than disappearing whenever one corner crosses the eye/near plane.
    PreviewVertex clipped[4];
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        const auto& a = vertices[(i+2)%3];
        const auto& b = vertices[i];
        if ((a.view.z >= near) != (b.view.z >= near)) {
            const float t = (near-a.view.z)/(b.view.z-a.view.z);
            clipped[count] = interpolate(a, b, t);
            clipped[count++].view.z = near;
        }
        if (b.view.z >= near) clipped[count++] = b;
    }
    for (int tri = 1; tri+1 < count; ++tri) {
        ScreenVertex p[3];
        const int indices[3]{0, tri, tri+1};
        bool finite = true;
        for (int i = 0; i < 3; ++i) {
            const auto& v = clipped[indices[i]];
            const float q = 1.0f/v.view.z;
            p[i] = {camera.cx-camera.focal*v.view.x*q,
                    camera.cy-camera.focal*v.view.y*q, q,
                    v.uv.u*q, v.uv.v*q, v.shade*q};
            finite &= std::isfinite(p[i].x) && std::isfinite(p[i].y);
        }
        if (!finite) continue;
        // Camera-relative source bounds keep projected coordinates below
        // this limit; retain a guard before float-to-integer conversion.
        RasterPoint r[3];
        for (int i = 0; i < 3; ++i) {
            finite &= std::fabs(p[i].x) < 1e6f && std::fabs(p[i].y) < 1e6f;
            if (!finite) break;
            r[i] = {std::llround(p[i].x*256.0),std::llround(p[i].y*256.0)};
        }
        if (!finite) continue;
        int64_t area = edge(r[0], r[1], r[2].x, r[2].y);
        if (!area) continue;
        // Two-sided: the offline viewer does not infer material cull flags.
        if (area < 0) { std::swap(p[1], p[2]); std::swap(r[1],r[2]); area = -area; }
        const float xmin = float(std::min({r[0].x,r[1].x,r[2].x}))/256;
        const float xmax = float(std::max({r[0].x,r[1].x,r[2].x}))/256;
        const float ymin = float(std::min({r[0].y,r[1].y,r[2].y}))/256;
        const float ymax = float(std::max({r[0].y,r[1].y,r[2].y}))/256;
        const int x0 = int(std::clamp(std::ceil(xmin-0.5f), 0.0f, float(width)));
        const int x1 = int(std::clamp(std::floor(xmax-0.5f), -1.0f, float(width-1)));
        const int y0 = int(std::clamp(std::ceil(ymin-0.5f), 0.0f, float(height)));
        const int y1 = int(std::clamp(std::floor(ymax-0.5f), -1.0f, float(height-1)));
        const int64_t bias[3]{top_left(r[1],r[2])?0:-1,top_left(r[2],r[0])?0:-1,top_left(r[0],r[1])?0:-1};
        const float lengths[3]{wireframe ? float(std::hypot(double(r[2].x-r[1].x),double(r[2].y-r[1].y)))*256 : 0,
                               wireframe ? float(std::hypot(double(r[0].x-r[2].x),double(r[0].y-r[2].y)))*256 : 0,
                               wireframe ? float(std::hypot(double(r[1].x-r[0].x),double(r[1].y-r[0].y)))*256 : 0};
        const float inv_area = 1.0f/float(area);
        const int64_t dx[3]{(r[1].y-r[2].y)*256,(r[2].y-r[0].y)*256,(r[0].y-r[1].y)*256};
        const int64_t dy[3]{(r[2].x-r[1].x)*256,(r[0].x-r[2].x)*256,(r[1].x-r[0].x)*256};
        int64_t row[3]{edge(r[1],r[2],x0*256+128,y0*256+128),
                       edge(r[2],r[0],x0*256+128,y0*256+128),
                       edge(r[0],r[1],x0*256+128,y0*256+128)};
        for (int y = y0; y <= y1; ++y, row[0]+=dy[0],row[1]+=dy[1],row[2]+=dy[2]) {
            int64_t e0=row[0],e1=row[1],e2=row[2];
            for (int x = x0; x <= x1; ++x,e0+=dx[0],e1+=dx[1],e2+=dx[2]) {
                if (e0+bias[0] < 0 || e1+bias[1] < 0 || e2+bias[2] < 0) continue;
                const float w0=float(e0)*inv_area, w1=float(e1)*inv_area, w2=float(e2)*inv_area;
                const float q = w0*p[0].q+w1*p[1].q+w2*p[2].q;
                const size_t at = size_t(y)*width+x;
                if (q <= depth[at]) continue;
                const float z = 1.0f/q;
                const uint8_t white[4]{255,255,255,255};
                const uint8_t* texel = white;
                if (texture) {
                    const float u = (w0*p[0].u+w1*p[1].u+w2*p[2].u)*z;
                    const float v = (w0*p[0].v+w1*p[1].v+w2*p[2].v)*z;
                    // Nearest sampling with repeat, matching the viewer's pages.
                    const int tx = std::min(int((u-std::floor(u))*texture->width), texture->width-1);
                    const int ty = std::min(int((v-std::floor(v))*texture->height), texture->height-1);
                    texel = &texture->rgba[(size_t(ty)*texture->width+tx)*4];
                }
                const unsigned alpha = texel[3];
                if (!alpha || (translucent ? alpha == 255 : alpha != 255)) continue;
                const float shade = std::clamp((w0*p[0].shade+w1*p[1].shade+w2*p[2].shade)*z,0.0f,1.0f);
                const bool line = wireframe &&
                    (e0 <= 0.7f*lengths[0] || e1 <= 0.7f*lengths[1] || e2 <= 0.7f*lengths[2]);
                for (int c = 0; c < 3; ++c) {
                    const unsigned color = line ? unsigned(c == 0 ? 12 : c == 1 ? 14 : 18)
                                                : unsigned(texel[c]*shade+0.5f);
                    rgba[at*4+c] = uint8_t((color*alpha+rgba[at*4+c]*(255-alpha)+127)/255);
                }
                if (!translucent) depth[at] = q;
            }
        }
    }
}

void render_preview(PreviewRaster& image, const Camera& camera,
                    const PreviewInstance* instances, size_t count,
                    float yaw, float pitch, bool lighting, bool wireframe) {
    struct Triangle { PreviewVertex v[3]; const TextureImage* texture; };
    std::vector<Triangle> transparent;
    const Vec3 light = light_direction();
    for (size_t instance = 0; instance < count; ++instance) {
        if (!instances[instance].part) continue;
        const Part& part = *instances[instance].part;
        const Mesh& mesh = part.mesh;
        const Transform& transform = instances[instance].transform;
        std::vector<PreviewVertex> vertices(mesh.positions.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            Vec3 p = transform.point(mesh.positions[i]);
            p = {p.x-camera.center.x,p.y-camera.center.y,p.z-camera.center.z};
            p = rotate_view(p,yaw,pitch);
            p.z += camera.distance;
            Vec3 n = rotate_view(normal(transform,mesh.normals[i]),yaw,pitch);
            const float length = std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
            const float shade = length > 1e-6f
                ? 0.30f+0.70f*std::fabs((n.x*light.x+n.y*light.y+n.z*light.z)/length) : 0.35f;
            vertices[i] = {p,{},shade};
        }
        const bool uvs = part.uvs.size() == mesh.indices.size();
        const bool pages = part.triangle_texture.size() == mesh.indices.size()/3;
        for (size_t t = 0; t < mesh.indices.size()/3; ++t) {
            Triangle triangle{};
            if (uvs && pages) {
                const uint32_t page = part.triangle_texture[t];
                if (page < part.textures.size() && !part.textures[page].empty())
                    triangle.texture = &part.textures[page];
            }
            for (size_t c = 0; c < 3; ++c) {
                triangle.v[c] = vertices[mesh.indices[t*3+c]];
                if (uvs) triangle.v[c].uv = part.uvs[t*3+c];
                if (triangle.texture && !lighting) triangle.v[c].shade = 1.0f;
            }
            image.triangle(camera,triangle.v,triangle.texture,false,wireframe);
            if (triangle.texture && triangle.texture->has_alpha)
                transparent.push_back(triangle);
        }
    }
    // Only genuinely translucent texels use painter order. All opaque and
    // cutout surfaces already share the same per-pixel depth buffer.
    std::stable_sort(transparent.begin(),transparent.end(),[](const Triangle& a,const Triangle& b) {
        return a.v[0].view.z+a.v[1].view.z+a.v[2].view.z > b.v[0].view.z+b.v[1].view.z+b.v[2].view.z;
    });
    for (const auto& t : transparent) image.triangle(camera,t.v,t.texture,true,wireframe);
}

} // namespace launcher_assets
