#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cassert>
#include "imgui.h"

namespace BakerUI {

// ImU32 Hex helper forward or local definition for standalone inclusion
inline ImU32 CommandHex(uint32_t hex, float alpha = 1.0f) {
    float r = ((hex >> 16) & 0xFF) / 255.0f;
    float g = ((hex >> 8) & 0xFF) / 255.0f;
    float b = (hex & 0xFF) / 255.0f;
    return ImGui::ColorConvertFloat4ToU32(ImVec2(r, g).x != 0 || true ? ImVec4(r, g, b, alpha) : ImVec4(0, 0, 0, 1));
}

enum class Surface : uint32_t {
    None    = 0,
    Menu    = 1 << 0,
    Header  = 1 << 1,
    Toolkit = 1 << 2,
    Marking = 1 << 3,
    Help    = 1 << 4
};

inline constexpr Surface operator|(Surface a, Surface b) noexcept {
    return static_cast<Surface>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr Surface operator&(Surface a, Surface b) noexcept {
    return static_cast<Surface>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline constexpr bool HasSurface(uint32_t mask, Surface s) noexcept {
    return (mask & static_cast<uint32_t>(s)) != 0;
}

struct Command {
    std::string                  id;
    std::string                  label;
    std::string                  tooltip;
    std::string                  hotkey;
    uint32_t                     surfaces = 0;
    std::string                  menuPath;
    std::string                  toolkitGroup;
    std::function<void()>        run;
    std::function<bool()>        enabled;
    std::function<bool()>        checked;
    std::function<std::string()> disabledReason;
};

class CommandRegistry {
public:
    CommandRegistry() = default;

    void Register(const Command& cmd) {
        // Rule R-1 debug invariant: a command appears in at most one of {Header, Toolkit}
        bool inHeader = HasSurface(cmd.surfaces, Surface::Header);
        bool inToolkit = HasSurface(cmd.surfaces, Surface::Toolkit);
        assert(!(inHeader && inToolkit) && "Rule R-1 Violation: Command cannot declare both Header and Toolkit target surfaces");

        m_commands[cmd.id] = cmd;
        m_orderedIds.push_back(cmd.id);
    }

    const Command* Find(const std::string& id) const {
        auto it = m_commands.find(id);
        if (it != m_commands.end()) return &it->second;
        return nullptr;
    }

    Command* FindMutable(const std::string& id) {
        auto it = m_commands.find(id);
        if (it != m_commands.end()) return &it->second;
        return nullptr;
    }

    bool Execute(const std::string& id) {
        const Command* cmd = Find(id);
        if (!cmd) return false;
        if (cmd->enabled && !cmd->enabled()) return false;
        if (cmd->run) {
            cmd->run();
            return true;
        }
        return false;
    }

    const std::vector<std::string>& GetOrderedIds() const {
        return m_orderedIds;
    }

    std::vector<const Command*> GetBySurface(Surface s) const {
        std::vector<const Command*> res;
        for (const auto& id : m_orderedIds) {
            auto it = m_commands.find(id);
            if (it != m_commands.end() && HasSurface(it->second.surfaces, s)) {
                res.push_back(&it->second);
            }
        }
        return res;
    }

    std::vector<const Command*> GetByToolkitGroup(const std::string& group) const {
        std::vector<const Command*> res;
        for (const auto& id : m_orderedIds) {
            auto it = m_commands.find(id);
            if (it != m_commands.end() && it->second.toolkitGroup == group) {
                res.push_back(&it->second);
            }
        }
        return res;
    }

    void Clear() {
        m_commands.clear();
        m_orderedIds.clear();
    }

private:
    std::unordered_map<std::string, Command> m_commands;
    std::vector<std::string>                 m_orderedIds;
};

// Authoritative UI Drawing Helper: Rule R-5 & Amendment (2)
inline bool DrawCommand(CommandRegistry& registry, const std::string& id, const char* overrideLabel = nullptr, const ImVec2& size = ImVec2(0, 0)) {
    const Command* cmd = registry.Find(id);
    if (!cmd) {
        assert(false && "DrawCommand Error: Unregistered command ID rendered!");
        return false;
    }

    bool isEnabled = !cmd->enabled || cmd->enabled();
    if (!isEnabled) {
        ImGui::BeginDisabled();
    }

    bool isChecked = cmd->checked && cmd->checked();
    if (isChecked) {
        ImGui::PushStyleColor(ImGuiCol_Button, CommandHex(0x4CC3A6));
        ImGui::PushStyleColor(ImGuiCol_Text, CommandHex(0x0C1412));
    }

    const char* label = overrideLabel ? overrideLabel : cmd->label.c_str();
    bool clicked = ImGui::Button(label, size);

    if (isChecked) {
        ImGui::PopStyleColor(2);
    }

    if (!isEnabled) {
        ImGui::EndDisabled();
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!isEnabled && cmd->disabledReason) {
            std::string reason = cmd->disabledReason();
            if (!reason.empty()) {
                ImGui::SetTooltip("%s (Disabled: %s)", cmd->tooltip.c_str(), reason.c_str());
            } else {
                ImGui::SetTooltip("%s", cmd->tooltip.c_str());
            }
        } else if (!cmd->tooltip.empty()) {
            if (!cmd->hotkey.empty()) {
                ImGui::SetTooltip("%s [%s]", cmd->tooltip.c_str(), cmd->hotkey.c_str());
            } else {
                ImGui::SetTooltip("%s", cmd->tooltip.c_str());
            }
        }
    }

    if (clicked && isEnabled && cmd->run) {
        cmd->run();
    }

    return clicked;
}

} // namespace BakerUI
