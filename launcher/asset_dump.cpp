// Small host-only JSON bridge for the asset conversion tooling. Keeping the
// memimage walk in asset_model.cpp means the exporter consumes the same
// source-backed POD, PCLT, and fail-closed texture decisions as the viewer.
#include "asset_model.h"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using launcher_assets::Mesh;
using launcher_assets::Model;
using launcher_assets::Part;
using launcher_assets::TextureImage;
using launcher_assets::Vec2;
using launcher_assets::Vec3;

const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::vector<uint8_t>& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t a = bytes[i];
        const uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const uint32_t word = (a << 16) | (b << 8) | c;
        out.push_back(kBase64[(word >> 18) & 63]);
        out.push_back(kBase64[(word >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kBase64[(word >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kBase64[word & 63] : '=');
    }
    return out;
}

void json_string(std::ostream& out, const std::string& value) {
    out << '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << unsigned(c) << std::dec << std::setfill(' ');
            } else {
                out << char(c);
            }
        }
    }
    out << '"';
}

void vec3(std::ostream& out, const Vec3& value) {
    out << '[' << value.x << ',' << value.y << ',' << value.z << ']';
}

void vec2(std::ostream& out, const Vec2& value) {
    out << '[' << value.u << ',' << value.v << ']';
}

void mesh_json(std::ostream& out, const Mesh& mesh) {
    out << "{\"positions\":[";
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        if (i) out << ',';
        vec3(out, mesh.positions[i]);
    }
    out << "],\"normals\":[";
    for (size_t i = 0; i < mesh.normals.size(); ++i) {
        if (i) out << ',';
        vec3(out, mesh.normals[i]);
    }
    out << "],\"indices\":[";
    for (size_t i = 0; i < mesh.indices.size(); ++i) {
        if (i) out << ',';
        out << mesh.indices[i];
    }
    out << "],\"bbox_min\":";
    vec3(out, mesh.bbox_min);
    out << ",\"bbox_max\":";
    vec3(out, mesh.bbox_max);
    out << '}';
}

void texture_json(std::ostream& out, const TextureImage& image) {
    out << "{\"width\":" << image.width << ",\"height\":"
        << image.height << ",\"has_alpha\":"
        << (image.has_alpha ? "true" : "false") << ",\"empty\":"
        << (image.empty() ? "true" : "false") << ",\"rgba_base64\":";
    json_string(out, image.empty() ? std::string{} : base64(image.rgba));
    out << '}';
}

void part_json(std::ostream& out, const Part& part) {
    out << "{\"label\":";
    json_string(out, part.label);
    out << ",\"component\":";
    json_string(out, part.component);
    out << ",\"body\":" << part.body << ",\"variant\":" << part.variant
        << ",\"lod\":" << part.lod << ",\"group_offset\":" << part.group_offset
        << ",\"chain_head\":" << part.chain_head
        << ",\"pod_count\":" << part.pod_count
        << ",\"vertex_count\":" << part.vertex_count
        << ",\"triangle_count\":" << part.triangle_count
        << ",\"mesh\":";
    mesh_json(out, part.mesh);
    out << ",\"uvs\":[";
    for (size_t i = 0; i < part.uvs.size(); ++i) {
        if (i) out << ',';
        vec2(out, part.uvs[i]);
    }
    out << "],\"triangle_texture\":[";
    for (size_t i = 0; i < part.triangle_texture.size(); ++i) {
        if (i) out << ',';
        out << part.triangle_texture[i];
    }
    out << "],\"triangle_clockwise\":[";
    for (size_t i = 0; i < part.triangle_clockwise.size(); ++i) {
        if (i) out << ',';
        out << int(part.triangle_clockwise[i]);
    }
    out << "],\"textures\":[";
    for (size_t i = 0; i < part.textures.size(); ++i) {
        if (i) out << ',';
        texture_json(out, part.textures[i]);
    }
    out << "],\"pod_pcase\":[";
    for (size_t i = 0; i < part.pod_pcase.size(); ++i) {
        if (i) out << ',';
        out << part.pod_pcase[i];
    }
    out << "],\"pod_offsets\":[";
    for (size_t i = 0; i < part.pod_offsets.size(); ++i) {
        if (i) out << ',';
        out << part.pod_offsets[i];
    }
    out << "],\"pod_ptexture\":[";
    for (size_t i = 0; i < part.pod_ptexture.size(); ++i) {
        if (i) out << ',';
        out << part.pod_ptexture[i];
    }
    out << "],\"pod_pmaterial\":[";
    for (size_t i = 0; i < part.pod_pmaterial.size(); ++i) {
        if (i) out << ',';
        out << part.pod_pmaterial[i];
    }
    out << "]}";
}

void model_json(std::ostream& out, const Model& model,
                const std::string& source) {
    out << std::setprecision(9);
    out << "{\"name\":";
    json_string(out, model.name);
    out << ",\"source\":";
    json_string(out, source);
    out << ",\"vertex_field_role\":\"dynamic_lit_normal_or_prelit_color\""
        << ",\"uv_layout\":\"triangle_corners\""
        << ",\"race_pack\":" << (model.race_pack ? "true" : "false")
        << ",\"default_part\":" << model.default_part
        << ",\"rejected_pods\":" << model.rejected_pods
        << ",\"texture_pages_decoded\":" << model.texture_pages_decoded
        << ",\"texture_pages_failed\":" << model.texture_pages_failed
        << ",\"untextured_pods\":" << model.untextured_pods
        << ",\"texture_error\":";
    json_string(out, model.texture_error);
    out << ",\"parts\":[";
    for (size_t i = 0; i < model.parts.size(); ++i) {
        if (i) out << ',';
        part_json(out, model.parts[i]);
    }
    out << "]}\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: m2_launcher_assets_dump MEMIMAGE [NAME]\n";
        return 2;
    }
    const std::filesystem::path path(argv[1]);
    const std::string name = argc == 3 ? argv[2] : path.stem().string();
    Model model;
    std::string error;
    if (!launcher_assets::load_model(path, name, model, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    model_json(std::cout, model, path.string());
    return 0;
}
