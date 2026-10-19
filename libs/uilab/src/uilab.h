// uilab.h - umbrella header for consumers that embed the lab.
//
// Link UILab::Core for the component model, theme and widget library.
// UILab::Editor (dockable panels) and UILab::Demos (paradigm pages) are pulled in
// by their own headers below, because they need targets from this tree.
#pragma once

#include "core/AppContext.h"
#include "core/Dialogs.h"
#include "core/EventLog.h"
#include "core/Notifications.h"
#include "core/Registry.h"
#include "core/Serialize.h"
#include "core/Settings.h"
#include "core/Types.h"
#include "core/UndoStack.h"
#include "core/Validation.h"
#include "themes/Theme.h"
#include "ui/Icons.h"
#include "ui/Texture.h"
#include "ui/Widgets.h"

#define UILAB_VERSION_MAJOR 1
#define UILAB_VERSION_MINOR 0
#define UILAB_VERSION_PATCH 0

namespace uilab {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

} // namespace uilab
