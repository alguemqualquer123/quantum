// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PLUGIN_INTERFACE_H
#define QUANTUM_PLUGIN_INTERFACE_H

#include "Export.h"
#include "PluginAbi.h"
#include "PluginContext.h"
#include "PluginUiSchema.h"

#include <string>

namespace quantum {
namespace plugin {

    // The contract every plugin implements. Nothing here is allowed to throw: the engine calls
    // through this vtable from a module that may be built with a different exception policy, and
    // _HAS_EXCEPTIONS=0 reaches the engine from Jolt (engine/libs/joltphysics/CMakeLists.txt:547).
    // Report failure with a return value and write the reason through PluginContext::log.
    class IPlugin {
    public:
        virtual ~IPlugin() = default;

        // Cross-checked against plugin.json after load. A mismatch is reported rather than fatal,
        // because the manifest is what the loader reads before the binary is even mapped.
        virtual const char* pluginId() const { return ""; }
        virtual const char* pluginVersion() const { return "0.0.0"; }
        virtual unsigned int pluginAbiVersion() const { return QUANTUM_PLUGIN_ABI_VERSION; }

        virtual bool initialize(PluginContext& context) { (void)context; return true; }
        virtual void shutdown() {}

        virtual bool activate() { return true; }
        virtual void deactivate() {}

        // Describes the plugin's panel. The editor calls this and renders the result with its own
        // ImGui context (see the rationale in PluginUiSchema.h).
        virtual void buildUiSchema(UiSchemaBuilder& builder) { (void)builder; }

        // ABI 2. Describes one of the windows this plugin registered through
        // PluginContext::registerWindow, called by the editor every frame that window is open. The
        // plugin still owns the layout and the editor owns the pixels: no ImGui type crosses this
        // call, which is why a plugin built without ImGui at all can have a window.
        // `windowId` is the id given to registerWindow, so one plugin may host several windows in
        // one method. A window nobody describes draws an empty panel, and the manager reports it.
        virtual void onDrawWindow(const std::string& windowId, UiSchemaBuilder& builder) {
            (void)windowId;
            (void)builder;
        }

        // ABI 2. The read side of the same frame: the whole window state as values (by field id)
        // plus the ids of the buttons pressed. The editor calls this only when something in the
        // window actually changed, so a plugin does not have to diff the map itself.
        virtual void onWindowInput(const std::string& windowId, const UiValueMap& values,
                                   const std::vector<std::string>& pressedButtons) {
            (void)windowId;
            (void)values;
            (void)pressedButtons;
        }

        // False means the command is unknown to this plugin; `result` is then left untouched.
        virtual bool executeCommand(const std::string& command, const UiValueMap& arguments,
                                    UiValue& result) {
            (void)command;
            (void)arguments;
            (void)result;
            return false;
        }

        // Called once per frame from the variable-timestep phase, after every scene has updated and
        // before the frame is drawn, so a plugin sees the same delta the gameplay did. The manager
        // only asks plugins whose manifest was granted the "tick" permission; a plugin that implements
        // this and does not declare "tick" is reported once in the plugin log.
        // `deltaTime` is the capped frame delta in seconds (Engine::getDeltatime), never a raw one.
        virtual void onUpdate(float deltaTime) { (void)deltaTime; }

        virtual void onSettingChanged(const std::string& key, const UiValue& value) {
            (void)key;
            (void)value;
        }
    };

    // Optional convenience base: keeps the context the manager handed in. Deriving from it is not
    // required, and a plugin that derives gets the same lifecycle calls.
    class QUANTUM_API PluginBase : public IPlugin {
    public:
        bool initialize(PluginContext& context) override;

        // Only valid after initialize() has run; the manager never calls anything else first.
        PluginContext& context() const;

    protected:
        PluginContext* pluginContext = nullptr;
    };

}
}

#endif // QUANTUM_PLUGIN_INTERFACE_H
