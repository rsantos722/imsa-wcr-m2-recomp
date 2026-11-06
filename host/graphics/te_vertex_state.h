// te_vertex_state.h — M2 Triangle Engine setup-engine vertex retention.
//
// The state transitions below are intentionally small and independent of the
// host rasterizer. They are derived from m2_te_device::add_vertex() in MAME's
// src/mame/konami/3dom2_te.cpp: a NEW flag resets the incomplete triangle,
// three vertices establish OMN, and later vertices replace one retained slot
// according to RM and the current tsort value.
//
// Derived from MAME (https://github.com/mamedev/mame), 3dom2_te.cpp.
// license:BSD-3-Clause
// copyright-holders:Philip Bennett
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
#pragma once

#include <array>
#include <cstdint>

namespace m2host {

enum class TeVertexSort : uint8_t {
    invalid = 0,
    omn = 1,
    mno = 2,
    onm = 3,
    nom = 4,
    mon = 5,
    nmo = 6,
};

template <typename Vertex>
class TeVertexState {
public:
    using Triangle = std::array<Vertex, 3>;

    TeVertexState() { reset(); }

    void reset()
    {
        m_slots = {};
        m_vcnt = 0;
        m_tsort = TeVertexSort::invalid;
    }

    // Add one setup-engine vertex. `new_triangle` is consumed by this one
    // vertex, matching the hardware interpreter's per-vertex flag clearing.
    // Returns true and writes `triangle` whenever the retained state contains
    // three valid vertices after the operation.
    bool add_vertex(const Vertex& vertex, bool new_triangle, bool rm,
                    Triangle* triangle = nullptr)
    {
        if (new_triangle) reset();

        if (m_vcnt < 3u) {
            m_slots[m_vcnt++] = vertex;
            if (m_vcnt == 3u) m_tsort = TeVertexSort::omn;
        } else if (rm) {
            switch (m_tsort) {
            case TeVertexSort::omn:
                m_slots[1] = vertex;
                m_tsort = TeVertexSort::onm;
                break;
            case TeVertexSort::mno:
                m_slots[0] = vertex;
                m_tsort = TeVertexSort::nmo;
                break;
            case TeVertexSort::onm:
                m_slots[2] = vertex;
                m_tsort = TeVertexSort::omn;
                break;
            case TeVertexSort::nom:
                m_slots[2] = vertex;
                m_tsort = TeVertexSort::mon;
                break;
            case TeVertexSort::mon:
                m_slots[0] = vertex;
                m_tsort = TeVertexSort::nom;
                break;
            case TeVertexSort::nmo:
                m_slots[1] = vertex;
                m_tsort = TeVertexSort::mno;
                break;
            case TeVertexSort::invalid:
                // A full setup state always has a valid tsort. Keep the
                // retained vertices unchanged if a caller presents an
                // impossible state rather than inventing a replacement.
                break;
            }
        } else {
            switch (m_tsort) {
            case TeVertexSort::omn:
                m_slots[0] = vertex;
                m_tsort = TeVertexSort::nom;
                break;
            case TeVertexSort::mno:
                m_slots[2] = vertex;
                m_tsort = TeVertexSort::omn;
                break;
            case TeVertexSort::onm:
                m_slots[0] = vertex;
                m_tsort = TeVertexSort::nmo;
                break;
            case TeVertexSort::nom:
                m_slots[1] = vertex;
                m_tsort = TeVertexSort::mno;
                break;
            case TeVertexSort::mon:
                m_slots[1] = vertex;
                m_tsort = TeVertexSort::onm;
                break;
            case TeVertexSort::nmo:
                m_slots[2] = vertex;
                m_tsort = TeVertexSort::mon;
                break;
            case TeVertexSort::invalid:
                break;
            }
        }

        if (m_vcnt != 3u) return false;
        if (triangle) *triangle = m_slots;
        return true;
    }

    uint32_t vertex_count() const { return m_vcnt; }
    TeVertexSort sort() const { return m_tsort; }
    const std::array<Vertex, 3>& slots() const { return m_slots; }

private:
    std::array<Vertex, 3> m_slots{};
    uint32_t m_vcnt = 0;
    TeVertexSort m_tsort = TeVertexSort::invalid;
};

}  // namespace m2host
