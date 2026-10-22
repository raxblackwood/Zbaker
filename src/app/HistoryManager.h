#pragma once

#include "../baker/MeshTypes.h"
#include "../uv/UVSurfaceCurve.h"
#include <vector>
#include <string>
#include <utility>

namespace Baker {

struct HistorySnapshot {
    std::string description;
    LowPolyMesh mesh;
    std::vector<SurfaceCurve> curves;
    int activeCurveIndex = -1;
};

class HistoryManager {
public:
    explicit HistoryManager(size_t maxStates = 40) : m_maxStates(maxStates), m_currentIndex(0) {}

    void PushState(const std::string& desc, const LowPolyMesh& mesh, const CurveManager& curveMgr) {
        // If we performed an undo and are now pushing a new state, discard forward redo states
        if (!m_states.empty() && m_currentIndex + 1 < m_states.size()) {
            m_states.erase(m_states.begin() + (m_currentIndex + 1), m_states.end());
        }

        HistorySnapshot snapshot;
        snapshot.description = desc;
        snapshot.mesh = mesh;
        snapshot.curves = curveMgr.curves;
        snapshot.activeCurveIndex = curveMgr.activeCurveIndex;

        m_states.push_back(std::move(snapshot));

        if (m_states.size() > m_maxStates) {
            m_states.erase(m_states.begin());
        }
        m_currentIndex = m_states.size() - 1;
    }

    bool CanUndo() const {
        return m_currentIndex > 0 && !m_states.empty();
    }

    bool CanRedo() const {
        return !m_states.empty() && (m_currentIndex + 1 < m_states.size());
    }

    bool Undo(LowPolyMesh& outMesh, CurveManager& outCurveMgr, std::string& outUndoneDesc) {
        if (!CanUndo()) return false;
        outUndoneDesc = m_states[m_currentIndex].description;
        m_currentIndex--;
        const auto& state = m_states[m_currentIndex];
        outMesh = state.mesh;
        outCurveMgr.curves = state.curves;
        outCurveMgr.activeCurveIndex = state.activeCurveIndex;
        return true;
    }

    bool Redo(LowPolyMesh& outMesh, CurveManager& outCurveMgr, std::string& outRedoneDesc) {
        if (!CanRedo()) return false;
        m_currentIndex++;
        const auto& state = m_states[m_currentIndex];
        outMesh = state.mesh;
        outCurveMgr.curves = state.curves;
        outCurveMgr.activeCurveIndex = state.activeCurveIndex;
        outRedoneDesc = state.description;
        return true;
    }

    void Clear() {
        m_states.clear();
        m_currentIndex = 0;
    }

    const std::string& GetUndoDescription() const {
        static const std::string empty;
        if (CanUndo() && m_currentIndex < m_states.size()) {
            return m_states[m_currentIndex].description;
        }
        return empty;
    }

    const std::string& GetRedoDescription() const {
        static const std::string empty;
        if (CanRedo() && (m_currentIndex + 1) < m_states.size()) {
            return m_states[m_currentIndex + 1].description;
        }
        return empty;
    }

    size_t GetStateCount() const { return m_states.size(); }
    size_t GetCurrentIndex() const { return m_currentIndex; }

private:
    std::vector<HistorySnapshot> m_states;
    size_t m_currentIndex = 0;
    size_t m_maxStates = 40;
};

} // namespace Baker
