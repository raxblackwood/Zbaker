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

// Button grammar (R10): Action is neutral, Primary is filled accent (Auto UV footer only),
// Destructive is red text on neutral fill.
enum class CommandStyle { Action, Primary, Destructive };

// Shared tooltip: label line, hotkey, and the disabled reason when the command cannot run.
inline void DrawCommandTooltip(const Command& cmd, bool isEnabled) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) return;
    std::string reason = (!isEnabled && cmd.disabledReason) ? cmd.disabledReason() : std::string();
    if (cmd.tooltip.empty() && reason.empty()) return;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(cmd.tooltip.c_str());
    if (!cmd.hotkey.empty()) ImGui::TextDisabled("Shortcut: %s", cmd.hotkey.c_str());
    if (!reason.empty()) ImGui::TextColored(ImVec4(0.94f, 0.62f, 0.04f, 1.0f), "Disabled: %s", reason.c_str());
    ImGui::EndTooltip();
}

// Authoritative UI Drawing Helper: Rule R-5 & Amendment (2)
inline bool DrawCommand(CommandRegistry& registry, const std::string& id, const char* overrideLabel = nullptr,
                        const ImVec2& size = ImVec2(0, 0), CommandStyle style = CommandStyle::Action) {
    const Command* cmd = registry.Find(id);
    if (!cmd) {
        assert(false && "DrawCommand Error: Unregistered command ID rendered!");
        return false;
    }

    bool isEnabled = !cmd->enabled || cmd->enabled();
    if (!isEnabled) {
        ImGui::BeginDisabled();
    }

    int pushedColors = 0;
    bool isChecked = cmd->checked && cmd->checked();
    if (style == CommandStyle::Primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, CommandHex(0x4CC3A6));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CommandHex(0x5FD2B6));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, CommandHex(0x3EB496));
        ImGui::PushStyleColor(ImGuiCol_Text, CommandHex(0x0C1412));
        pushedColors = 4;
    } else if (isChecked) {
        ImGui::PushStyleColor(ImGuiCol_Button, CommandHex(0x4CC3A6));
        ImGui::PushStyleColor(ImGuiCol_Text, CommandHex(0x0C1412));
        pushedColors = 2;
    } else if (style == CommandStyle::Destructive) {
        ImGui::PushStyleColor(ImGuiCol_Text, CommandHex(0xEF4444));
        pushedColors = 1;
    }

    // "##id" suffix keeps ImGui IDs unique when two commands share a visible label
    std::string label = std::string(overrideLabel ? overrideLabel : cmd->label.c_str()) + "##" + cmd->id;
    bool clicked = ImGui::Button(label.c_str(), size);

    if (pushedColors) {
        ImGui::PopStyleColor(pushedColors);
    }

    if (!isEnabled) {
        ImGui::EndDisabled();
    }

    DrawCommandTooltip(*cmd, isEnabled);

    if (clicked && isEnabled && cmd->run) {
        cmd->run();
    }

    return clicked;
}

// Menu-bar counterpart of DrawCommand: label, hotkey, checked and enabled state all come from the registry.
inline bool DrawCommandMenuItem(CommandRegistry& registry, const std::string& id) {
    const Command* cmd = registry.Find(id);
    if (!cmd) {
        assert(false && "DrawCommandMenuItem Error: Unregistered command ID rendered!");
        return false;
    }

    bool isEnabled = !cmd->enabled || cmd->enabled();
    bool isChecked = cmd->checked && cmd->checked();
    std::string label = cmd->label + "##" + cmd->id;
    bool clicked = ImGui::MenuItem(label.c_str(), cmd->hotkey.empty() ? nullptr : cmd->hotkey.c_str(), isChecked, isEnabled);
    DrawCommandTooltip(*cmd, isEnabled);

    if (clicked && isEnabled && cmd->run) {
        cmd->run();
    }
    return clicked;
}

// Draws every Menu-surface command whose menuPath is `path` or nested below it, in registration order.
// With inlineGroups, first-level sub-paths render as labelled sections ("UV/Cut & Sew"); deeper levels
// (and all levels when inlineGroups is false) render as submenus ("View/3D Display").
inline void DrawCommandMenuContents(CommandRegistry& registry, const std::string& path, bool inlineGroups) {
    struct Entry { const Command* cmd; std::string group; };
    std::vector<Entry> entries;
    const std::string prefix = path + "/";
    for (const Command* cmd : registry.GetBySurface(Surface::Menu)) {
        if (cmd->menuPath == path) {
            entries.push_back({ cmd, "" });
        } else if (cmd->menuPath.compare(0, prefix.size(), prefix) == 0) {
            std::string rest = cmd->menuPath.substr(prefix.size());
            std::string group = rest.substr(0, rest.find('/'));
            bool seen = false;
            for (const Entry& e : entries) {
                if (!e.cmd && e.group == group) { seen = true; break; }
            }
            if (!seen) entries.push_back({ nullptr, group });
        }
    }

    bool prevWasSection = false;
    for (const Entry& e : entries) {
        if (e.cmd) {
            if (prevWasSection) ImGui::Separator();
            DrawCommandMenuItem(registry, e.cmd->id);
            prevWasSection = false;
        } else if (inlineGroups) {
            ImGui::SeparatorText(e.group.c_str());
            DrawCommandMenuContents(registry, prefix + e.group, false);
            prevWasSection = true;
        } else {
            if (ImGui::BeginMenu(e.group.c_str())) {
                DrawCommandMenuContents(registry, prefix + e.group, false);
                ImGui::EndMenu();
            }
            prevWasSection = false;
        }
    }
}

} // namespace BakerUI
