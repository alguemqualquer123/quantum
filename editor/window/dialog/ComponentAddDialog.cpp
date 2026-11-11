// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "ComponentAddDialog.h"
#include "external/IconsFontAwesome6.h"
#include "util/UIUtils.h"
#include <cctype>

namespace quantum {
namespace editor {

namespace {

struct ReadyMadeStyle {
    const char* prefix;
    const char* icon;
    ImVec4 color;
};

// The library's own groups, styled here so a component only has to name its category in the
// "-- category:" header line. Matching is by prefix, so "Water & Shore" lands on "Water".
const ReadyMadeStyle READY_MADE_STYLES[] = {
    {"Water", ICON_FA_WATER, ImVec4(0.30f, 0.65f, 0.90f, 1.0f)},
    {"Physics", ICON_FA_ATOM, ImVec4(0.70f, 0.55f, 0.90f, 1.0f)},
    {"Environment", ICON_FA_SUN, ImVec4(0.45f, 0.80f, 0.60f, 1.0f)},
    {"Camera", ICON_FA_VIDEO, ImVec4(0.50f, 0.70f, 0.90f, 1.0f)},
    {"Interaction", ICON_FA_HEART, ImVec4(0.90f, 0.45f, 0.50f, 1.0f)},
    {"Scene", ICON_FA_LAYER_GROUP, ImVec4(0.55f, 0.80f, 0.45f, 1.0f)},
    {"Audio", ICON_FA_MUSIC, ImVec4(0.95f, 0.70f, 0.35f, 1.0f)},
    {"Screen", ICON_FA_BOLT, ImVec4(0.95f, 0.90f, 0.45f, 1.0f)},
};

ReadyMadeStyle readyMadeStyle(const std::string& category) {
    for (const ReadyMadeStyle& style : READY_MADE_STYLES) {
        if (category.rfind(style.prefix, 0) == 0) {
            return style;
        }
    }
    return ReadyMadeStyle{ nullptr, ICON_FA_CODE, ImVec4(0.70f, 0.50f, 0.80f, 1.0f) };
}

void drawCategoryHeader(const char* icon, const char* name, ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::SetCursorPosX(10);
    ImGui::Text("%s %s", icon, name);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// One row of the list: a full-width button with an icon, a name and a wrapped description
// drawn over it. Both the component types and the ready-made scripts use it.
bool drawEntryRow(int index, const char* icon, ImVec4 color, const std::string& name,
                  const std::string& description) {
    ImGui::PushID(index);

    const ImVec2 cursorPos = ImGui::GetCursorPos();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.235f, 0.314f, 0.471f, 0.392f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.314f, 0.392f, 0.549f, 0.588f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.0f));

    const bool clicked = ImGui::Button("##component", ImVec2(ImGui::GetContentRegionAvail().x, 45));

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImGui::SetCursorPos(ImVec2(cursorPos.x + 20, cursorPos.y + 5));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Text("%s", icon);
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::SetCursorPosX(cursorPos.x + 50);

    ImGui::BeginGroup();
    ImGui::Text("%s", name.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3);
    ImGui::TextWrapped("%s", description.c_str());
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    ImGui::PopID();
    return clicked;
}

bool matchesSearch(const std::string& name, const std::string& search) {
    if (search.empty()) {
        return true;
    }
    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find(search) != std::string::npos;
}

} // namespace

ComponentAddDialog::ComponentAddDialog() {
    initializeCategories();
}

void ComponentAddDialog::setReadyMadeComponents(
    std::vector<ReadyMadeCategory> categories,
    std::function<void(const std::string&)> onAdd,
    std::function<bool(const std::string&)> canAdd) {

    m_readyMade = std::move(categories);
    m_onAddReadyMade = std::move(onAdd);
    m_canAddReadyMade = std::move(canAdd);
}

void ComponentAddDialog::initializeCategories() {
    m_categories = {
        {"Core", ICON_FA_LOCATION_CROSSHAIRS, ImVec4(0.4f, 0.8f, 0.9f, 1.0f), {
            {ComponentType::Transform, ICON_FA_UP_DOWN_LEFT_RIGHT, "Transform", "Position, rotation, scale and hierarchy", ImVec4(0.4f, 0.8f, 0.9f, 1.0f)},
            {ComponentType::NetworkComponent, ICON_FA_NETWORK_WIRED, "Network", "Replicated entity identity and transform policy", ImVec4(0.4f, 0.7f, 0.9f, 1.0f)}
        }},
        {"Rendering", ICON_FA_CUBE, ImVec4(0.9f, 0.5f, 0.3f, 1.0f), {
            {ComponentType::MeshComponent, ICON_FA_SHAPES, "Mesh", "3D mesh renderer", ImVec4(0.9f, 0.5f, 0.3f, 1.0f)},
            {ComponentType::LightComponent, ICON_FA_LIGHTBULB, "Light", "Light source", ImVec4(1.0f, 0.9f, 0.3f, 1.0f)},
            {ComponentType::CameraComponent, ICON_FA_VIDEO, "Camera", "Camera view", ImVec4(0.5f, 0.7f, 0.9f, 1.0f)},
            {ComponentType::SkyComponent, ICON_FA_CLOUD_SUN, "Sky", "Skybox or skydome", ImVec4(0.6f, 0.8f, 1.0f, 1.0f)},
            {ComponentType::FogComponent, ICON_FA_SMOG, "Fog", "Fog effect", ImVec4(0.7f, 0.7f, 0.8f, 1.0f)},
            {ComponentType::MirrorComponent, ICON_FA_CLONE, "Mirror", "Planar reflection surface", ImVec4(0.6f, 0.8f, 0.9f, 1.0f)},
            {ComponentType::ReflectionProbeComponent, ICON_FA_CUBE, "Reflection Probe", "Local static or dynamic environment reflection", ImVec4(0.45f, 0.75f, 1.0f, 1.0f)},
            {ComponentType::DecalComponent, ICON_FA_IMAGE, "Decal", "Projected texture, colour and edge fade on scene geometry", ImVec4(0.95f, 0.55f, 0.35f, 1.0f)},
            {ComponentType::WaterComponent, ICON_FA_WATER, "Water", "Water surface with waves, reflections and foam", ImVec4(0.3f, 0.65f, 0.9f, 1.0f)},
            {ComponentType::WaterExclusionComponent, ICON_FA_DROPLET_SLASH, "Water Exclusion", "Keeps water out of a volume, like a boat", ImVec4(0.95f, 0.6f, 0.25f, 1.0f)},
            {ComponentType::ModelComponent, ICON_FA_CUBES, "Model", "3D model", ImVec4(0.8f, 0.4f, 0.4f, 1.0f)},
            {ComponentType::TerrainComponent, ICON_FA_MOUNTAIN, "Terrain", "Terrain mesh", ImVec4(0.4f, 0.7f, 0.3f, 1.0f)},
            {ComponentType::InstancedMeshComponent, ICON_FA_CLONE, "Instanced Mesh", "Instanced rendering", ImVec4(0.7f, 0.4f, 0.5f, 1.0f)}
        }},
        {"User Interface", ICON_FA_WINDOW_RESTORE, ImVec4(0.5f, 0.3f, 0.9f, 1.0f), {
            {ComponentType::UIComponent, ICON_FA_SQUARE, "UI Base", "Basic UI element", ImVec4(0.5f, 0.3f, 0.9f, 1.0f)},
            {ComponentType::UILayoutComponent, ICON_FA_TABLE_CELLS, "UI Layout", "Layout container", ImVec4(0.6f, 0.4f, 0.9f, 1.0f)},
            {ComponentType::UIContainerComponent, ICON_FA_BOX, "UI Container", "Container element", ImVec4(0.55f, 0.35f, 0.85f, 1.0f)},
            {ComponentType::ImageComponent, ICON_FA_IMAGE, "Image", "UI image display", ImVec4(0.7f, 0.5f, 0.9f, 1.0f)},
            {ComponentType::TextComponent, ICON_FA_FONT, "Text", "Text display", ImVec4(0.8f, 0.6f, 0.9f, 1.0f)},
            {ComponentType::ButtonComponent, ICON_FA_SQUARE_CHECK, "Button", "Interactive button", ImVec4(0.5f, 0.7f, 0.5f, 1.0f)},
            {ComponentType::PanelComponent, ICON_FA_WINDOW_MAXIMIZE, "Panel", "UI panel", ImVec4(0.4f, 0.5f, 0.7f, 1.0f)},
            {ComponentType::ScrollbarComponent, ICON_FA_ARROWS_UP_DOWN, "Scrollbar", "Scroll control", ImVec4(0.6f, 0.6f, 0.7f, 1.0f)},
            {ComponentType::ProgressbarComponent, ICON_FA_BARS_PROGRESS, "Progressbar", "Progress indicator", ImVec4(0.4f, 0.7f, 0.5f, 1.0f)},
            {ComponentType::TextEditComponent, ICON_FA_KEYBOARD, "Text Edit", "Text input field", ImVec4(0.7f, 0.7f, 0.8f, 1.0f)}
        }},
        {"2D Graphics", ICON_FA_LAYER_GROUP, ImVec4(0.3f, 0.8f, 0.5f, 1.0f), {
            {ComponentType::SpriteComponent, ICON_FA_IMAGE, "Sprite", "2D sprite", ImVec4(0.3f, 0.8f, 0.5f, 1.0f)},
            {ComponentType::Light2DComponent, ICON_FA_LIGHTBULB, "2D Light", "2D point light with radius falloff", ImVec4(1.0f, 0.9f, 0.3f, 1.0f)},
            {ComponentType::Occluder2DComponent, ICON_FA_CIRCLE_HALF_STROKE, "2D Occluder", "Casts shadows from 2D lights", ImVec4(0.5f, 0.5f, 0.6f, 1.0f)},
            {ComponentType::SpriteAnimationComponent, ICON_FA_FILM, "Sprite Animation", "Animated sprite", ImVec4(0.4f, 0.8f, 0.6f, 1.0f)},
            {ComponentType::TilemapComponent, ICON_FA_BORDER_ALL, "Tilemap", "Tile-based map", ImVec4(0.5f, 0.7f, 0.6f, 1.0f)},
            {ComponentType::PolygonComponent, ICON_FA_DRAW_POLYGON, "Polygon", "2D polygon shape", ImVec4(0.6f, 0.8f, 0.4f, 1.0f)},
            {ComponentType::MeshPolygonComponent, ICON_FA_VECTOR_SQUARE, "Mesh Polygon", "Mesh-based polygon", ImVec4(0.5f, 0.75f, 0.45f, 1.0f)}
        }},
        {"Primitives", ICON_FA_SHAPES, ImVec4(0.7f, 0.4f, 0.7f, 1.0f), {
            {ComponentType::PointsComponent, ICON_FA_BRAILLE, "Points", "Point cloud", ImVec4(0.8f, 0.5f, 0.8f, 1.0f)},
            {ComponentType::LinesComponent, ICON_FA_GRIP_LINES, "Lines", "Line renderer", ImVec4(0.7f, 0.4f, 0.7f, 1.0f)},
            {ComponentType::ParticlesComponent, ICON_FA_FIRE, "Particles", "Particle system", ImVec4(0.9f, 0.3f, 0.5f, 1.0f)}
        }},
        {"Physics", ICON_FA_ATOM, ImVec4(0.8f, 0.3f, 0.3f, 1.0f), {
            {ComponentType::Body2DComponent, ICON_FA_CIRCLE, "2D Physics Body", "2D rigid body", ImVec4(0.8f, 0.3f, 0.3f, 1.0f)},
            {ComponentType::Body3DComponent, ICON_FA_CUBE, "3D Physics Body", "3D rigid body", ImVec4(0.9f, 0.4f, 0.4f, 1.0f)},
            {ComponentType::Joint2DComponent, ICON_FA_LINK, "2D Joint", "2D physics joint", ImVec4(0.7f, 0.5f, 0.3f, 1.0f)},
            {ComponentType::Joint3DComponent, ICON_FA_LINK, "3D Joint", "3D physics joint", ImVec4(0.8f, 0.6f, 0.4f, 1.0f)}
        }},
        {"Animation", ICON_FA_PLAY, ImVec4(0.9f, 0.6f, 0.2f, 1.0f), {
            {ComponentType::AnimationComponent, ICON_FA_FILM, "Animation", "Animation player", ImVec4(0.9f, 0.6f, 0.2f, 1.0f)},
            {ComponentType::AnimatorControllerComponent, ICON_FA_PERSON, "Animator Controller", "State machine with clips, audio and events", ImVec4(0.95f, 0.55f, 0.25f, 1.0f)},
            {ComponentType::ActionComponent, ICON_FA_PLAY, "Action", "Action sequence", ImVec4(0.8f, 0.7f, 0.3f, 1.0f)},
            {ComponentType::TimedActionComponent, ICON_FA_CLOCK, "Timed Action", "Time-based action", ImVec4(0.85f, 0.65f, 0.25f, 1.0f)},
            {ComponentType::AlphaActionComponent, ICON_FA_EYE, "Alpha Action", "Opacity animation", ImVec4(0.7f, 0.6f, 0.4f, 1.0f)},
            {ComponentType::ColorActionComponent, ICON_FA_PALETTE, "Color Action", "Color animation", ImVec4(0.8f, 0.5f, 0.3f, 1.0f)},
            {ComponentType::PositionActionComponent, ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Position Action", "Position animation", ImVec4(0.75f, 0.7f, 0.35f, 1.0f)},
            {ComponentType::RotationActionComponent, ICON_FA_ROTATE, "Rotation Action", "Rotation animation", ImVec4(0.85f, 0.75f, 0.3f, 1.0f)},
            {ComponentType::ScaleActionComponent, ICON_FA_EXPAND, "Scale Action", "Scale animation", ImVec4(0.9f, 0.65f, 0.35f, 1.0f)},
            {ComponentType::BoneComponent, ICON_FA_BONE, "Bone", "Skeletal bone", ImVec4(0.9f, 0.9f, 0.8f, 1.0f)},
            {ComponentType::KeyframeTracksComponent, ICON_FA_KEY, "Keyframe Tracks", "Keyframe animation", ImVec4(0.8f, 0.6f, 0.3f, 1.0f)},
            {ComponentType::MorphTracksComponent, ICON_FA_FACE_SMILE, "Morph Tracks", "Morph targets", ImVec4(0.7f, 0.65f, 0.4f, 1.0f)},
            {ComponentType::RotateTracksComponent, ICON_FA_CIRCLE_NOTCH, "Rotate Tracks", "Rotation tracks", ImVec4(0.75f, 0.6f, 0.35f, 1.0f)},
            {ComponentType::ScaleTracksComponent, ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Scale Tracks", "Scale tracks", ImVec4(0.8f, 0.55f, 0.3f, 1.0f)},
            {ComponentType::TranslateTracksComponent, ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Translate Tracks", "Translation tracks", ImVec4(0.85f, 0.6f, 0.25f, 1.0f)}
        }},
        {"Sound & Script", ICON_FA_CODE, ImVec4(0.6f, 0.6f, 0.6f, 1.0f), {
            {ComponentType::SoundComponent, ICON_FA_VOLUME_HIGH, "Sound", "Sound player", ImVec4(0.4f, 0.7f, 0.9f, 1.0f)},
            {ComponentType::ScriptComponent, ICON_FA_CODE, "Script", "C++ or Lua script", ImVec4(0.7f, 0.5f, 0.8f, 1.0f)}
        }}
    };
}

void ComponentAddDialog::open(
    std::function<void(ComponentType)> onAdd,
    std::function<bool(ComponentType)> canAddComponent,
    std::function<void()> onCancel
) {
    m_isOpen = true;
    m_justOpened = true;
    memset(m_searchBuffer, 0, sizeof(m_searchBuffer));
    m_hoveredIndex = -1;
    m_onAdd = onAdd;
    m_canAddComponent = canAddComponent;
    m_onCancel = onCancel;
}

void ComponentAddDialog::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Add new component##ComponentAddModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 0.0f), ImGuiCond_Once);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | 
                             ImGuiWindowFlags_Modal;

    if (!ImGui::BeginPopupModal("Add new component##ComponentAddModal", nullptr, flags)) {
        if (m_isOpen) {
            m_isOpen = false;
            if (m_onCancel) m_onCancel();
        }
        return;
    }

    // Search input
    UIUtils::searchInput("##search", "Search components...", m_searchBuffer, sizeof(m_searchBuffer), m_justOpened);
    m_justOpened = false;

    ImGui::Separator();

    // Filter components based on search
    std::string searchStr(m_searchBuffer);
    std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

    // Scrollable component list
    ImGui::BeginChild("ComponentList", ImVec2(0, 320), false);

    int rowIndex = 0;
    bool foundAny = false;
    bool addComponent = false;
    ComponentType addType = ComponentType::Transform;
    bool addReadyMade = false;
    std::string addReadyMadeName;

    ImGui::Dummy(ImVec2(0, 5));

    for (const auto& category : m_categories) {
        std::vector<const ComponentEntry*> filteredEntries;

        // Filter entries based on search and availability
        for (const auto& entry : category.entries) {
            if (!matchesSearch(entry.name, searchStr)) {
                continue;
            }

            // Check if component can be added
            if (m_canAddComponent && m_canAddComponent(entry.type)) {
                filteredEntries.push_back(&entry);
            }
        }

        if (filteredEntries.empty()) {
            continue;
        }

        foundAny = true;
        drawCategoryHeader(category.icon, category.name, category.color);

        for (const ComponentEntry* entry : filteredEntries) {
            if (drawEntryRow(rowIndex++, entry->icon, entry->color, entry->name, entry->description)) {
                addComponent = true;
                addType = entry->type;
            }
            ImGui::Dummy(ImVec2(0, 5));
        }
    }

    // Then the ready-made Lua components the engine ships, grouped by the "-- category:" line
    // each script declares. Some group names match an engine category ("Physics"), so the
    // section is labelled once instead of renaming every group.
    bool readyMadeHeadingDrawn = false;
    for (const auto& category : m_readyMade) {
        std::vector<const ReadyMadeEntry*> filteredEntries;

        for (const auto& entry : category.entries) {
            if (!matchesSearch(entry.name, searchStr)) {
                continue;
            }
            if (m_canAddReadyMade && !m_canAddReadyMade(entry.name)) {
                continue;
            }
            filteredEntries.push_back(&entry);
        }

        if (filteredEntries.empty()) {
            continue;
        }

        foundAny = true;
        if (!readyMadeHeadingDrawn) {
            readyMadeHeadingDrawn = true;
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::SeparatorText("Ready-made scripts");
        }
        const ReadyMadeStyle style = readyMadeStyle(category.name);
        drawCategoryHeader(style.icon, category.name.c_str(), style.color);

        for (const ReadyMadeEntry* entry : filteredEntries) {
            if (drawEntryRow(rowIndex++, style.icon, style.color, entry->name, entry->description)) {
                addReadyMade = true;
                addReadyMadeName = entry->name;
            }
            ImGui::Dummy(ImVec2(0, 5));
        }
    }

    if (!foundAny) {
        // No components available message
        ImGui::Dummy(ImVec2(0, 100));

        ImVec2 textSize = ImGui::CalcTextSize("No components available");
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - textSize.x) * 0.5f);
        ImGui::TextDisabled("No components available");

        if (searchStr.empty()) {
            textSize = ImGui::CalcTextSize("All components are already added");
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - textSize.x) * 0.5f);
            ImGui::TextDisabled("All components are already added");
        } else {
            textSize = ImGui::CalcTextSize("Try a different search term");
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - textSize.x) * 0.5f);
            ImGui::TextDisabled("Try a different search term");
        }

        ImGui::Dummy(ImVec2(0, 100));
    }

    ImGui::EndChild();

    // The chosen row is resolved after the list is drawn: the callbacks rebuild the inspector,
    // so running one inside the row would mutate the scene while the popup still references it.
    const bool chose = addComponent || addReadyMade;
    if (chose) {
        memset(m_searchBuffer, 0, sizeof(m_searchBuffer));
        m_hoveredIndex = -1;
        m_justOpened = false;
        m_isOpen = false;
        if (addComponent) {
            if (m_onAdd) m_onAdd(addType);
        } else if (m_onAddReadyMade) {
            m_onAddReadyMade(addReadyMadeName);
        }
        ImGui::CloseCurrentPopup();
    }

    if (!chose) {
        // Footer with Cancel button
        ImGui::Separator();

        float buttonWidth = 100;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5f);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));

        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            memset(m_searchBuffer, 0, sizeof(m_searchBuffer));
            m_hoveredIndex = -1;
            m_justOpened = false;
            m_isOpen = false;
            if (m_onCancel) m_onCancel();
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(2);
    }

    ImGui::EndPopup();
}

} // namespace editor
} // namespace quantum
