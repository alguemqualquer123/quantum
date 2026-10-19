// Icons.h - vector icons drawn with ImDrawList (no font or image assets needed).
#pragma once

#include <imgui.h>

namespace uilab {
namespace icons {

enum class Icon : int {
    None = 0, New, Open, Save, SaveAll, Undo, Redo, Play, Pause, Stop, Step, Grid, Search, Gear,
    Layers, Cube, Tree, List, Widget, Palette, Trash, Copy, Refresh, Eye, EyeOff, Lock, Unlock,
    ChevronDown, ChevronRight, ChevronUp, ChevronLeft, Close, Check, Warning, Info, Error, Star,
    Camera, Sun, Moon, Bug, Chart, Dock, PanelLeft, PanelRight, PanelBottom, Sliders, Link, Plus,
    Minus, Filter, Home, Folder, File, Image, Brush, Bolt, Heart, Shield, Coin, Backpack, Point,
    Count
};

const char* Name(Icon icon);

// Draws the icon centred at 'center' with a bounding box of 'size' pixels.
bool Draw(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 col, float thickness = 1.6f);

// Immediate-mode icon button: returns true when clicked.
bool IconButton(const char* id, Icon icon, const char* tooltip, ImVec2 size, bool active = false,
                bool enabled = true, float iconScale = 0.62f);

// Small toolbar-style button with an icon and an optional label.
bool IconTextButton(const char* id, Icon icon, const char* label, ImVec2 size, bool active = false,
                    bool enabled = true, const char* tooltip = nullptr);

// Draws icon + label on the current line (label drawn with ImGui::TextUnformatted).
void IconText(Icon icon, const char* label, ImU32 textColor);

// Returns the icon that represents an asset type (used by the asset browser).
Icon ForAssetType(int assetTypeIndex);

} // namespace icons
} // namespace uilab
