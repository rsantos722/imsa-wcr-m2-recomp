// te_reference_vertex_state.h — independent M2 setup-engine state model.
//
// This intentionally keeps the replacement table separate from the
// production TeVertexState switch.  It is used only for bounded diagnostics
// and tests, so a future production change cannot make the comparison agree
// with itself by construction.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "te_vertex_state.h"

namespace m2host {

template <typename Vertex>
class TeReferenceVertexState {
public:
    using Triangle = std::array<Vertex, 3>;

    TeReferenceVertexState() { reset(); }

    void reset()
    {
        m_slots = {};
        m_vcnt = 0;
        m_tsort = TeVertexSort::invalid;
    }

    bool add_vertex(const Vertex& vertex, bool new_triangle, bool rm,
                    Triangle* triangle = nullptr)
    {
        if (new_triangle) reset();

        if (m_vcnt < 3u) {
            m_slots[m_vcnt++] = vertex;
            if (m_vcnt == 3u) m_tsort = TeVertexSort::omn;
        } else {
            const Replacement replacement = replacement_for(m_tsort, rm);
            if (replacement.valid) {
                m_slots[replacement.slot] = vertex;
                m_tsort = replacement.next;
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
    struct Replacement {
        uint8_t slot = 0;
        TeVertexSort next = TeVertexSort::invalid;
        bool valid = false;
    };

    static Replacement replacement_for(TeVertexSort sort, bool rm)
    {
        // The index is the M2 tsort value: 1=OMN, 2=MNO, 3=ONM,
        // 4=NOM, 5=MON, 6=NMO.  Each entry is the slot replacement and
        // next sort state from the hardware setup-engine transition table.
        static constexpr Replacement strip[] = {
            {},
            {0, TeVertexSort::nom, true},
            {2, TeVertexSort::omn, true},
            {0, TeVertexSort::nmo, true},
            {1, TeVertexSort::mno, true},
            {1, TeVertexSort::onm, true},
            {2, TeVertexSort::mon, true},
        };
        static constexpr Replacement fan[] = {
            {},
            {1, TeVertexSort::onm, true},
            {0, TeVertexSort::nmo, true},
            {2, TeVertexSort::omn, true},
            {2, TeVertexSort::mon, true},
            {0, TeVertexSort::nom, true},
            {1, TeVertexSort::mno, true},
        };
        const size_t index = static_cast<size_t>(sort);
        if (index >= 7u) return {};
        return (rm ? fan : strip)[index];
    }

    std::array<Vertex, 3> m_slots{};
    uint32_t m_vcnt = 0;
    TeVertexSort m_tsort = TeVertexSort::invalid;
};

}  // namespace m2host
