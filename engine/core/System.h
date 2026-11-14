// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef SYSTEM_H
#define SYSTEM_H

#define D_LOG_VERBOSE 1
#define D_LOG_DEBUG 2
#define D_LOG_WARN 3
#define D_LOG_ERROR 4

#include "Export.h"
#include <stdio.h>
#include <string>
#include <vector>
#include "io/Data.h"
#include "sokol_gfx.h"

namespace quantum {
    class Engine;

    class AdMobBackend;
    class InAppPurchaseBackend;
    class WebPortalBackend;

    enum class CursorType{
        ARROW, // default
        IBEAM,
        CROSSHAIR,
        POINTING_HAND,
        RESIZE_EW,
        RESIZE_NS,
        RESIZE_NWSE,
        RESIZE_NESW,
        RESIZE_ALL,
        NOT_ALLOWED
    };

    // How the mouse cursor behaves. A single authoritative state replaces the old
    // independent "show cursor" and "mouse locked" flags: visibility and motion
    // confinement are two axes of one mode, so only the valid combinations exist.
    enum class MouseMode{
        NORMAL,   // visible, moves freely (default)
        HIDDEN,   // hidden, moves freely (absolute position still reported)
        CAPTURED, // hidden and locked to the window; relative motion only (mouse-look)
        CONFINED  // visible, but cannot leave the window bounds
    };

    class QUANTUM_API System {
    private:

        friend class Engine;

        static System* systemInstance;

        // Only Engine is allowed to inject an external System implementation.
        static void setSystemInstance(System* system);

    protected:

        System() {}

    public:
        std::vector<std::string> args;

        static System& instance();

        // True once Engine has installed the System. Log and resource helpers use this to stay
        // silent (instead of aborting) when something reaches them before Engine::systemInit.
        static bool hasInstance();

        virtual ~System() {}

        // *******
        // Used for user and Lua
        // *******
        virtual int getScreenWidth() = 0;
        virtual int getScreenHeight() = 0;

        virtual int getSampleCount();

        // Copy the window's colour buffer into `out` as width*height*4 bytes, RGBA, first row at the
        // top. Only ever called between a committed frame and the swap, and false where the platform
        // has no read path (FrameCapture logs the reason).
        virtual bool readBackFramebuffer(unsigned char* out, int width, int height);

        // Touch is the main input, as on phones and tablets; a laptop with a touch
        // screen and a mouse is not a touch device
        virtual bool isTouchDevice();

        virtual void showVirtualKeyboard(std::wstring text = L"");
        virtual void hideVirtualKeyboard();

        virtual bool isFullscreen();
        virtual void requestFullscreen();
        virtual void exitFullscreen();

        // Desktop window control. Maximize/resize/resizable are no-ops where
        // the platform offers no such control (mobile, web, sokol app backend);
        // setWindowTitle also applies on web (tab title) and sokol.
        // quit() closes the app, except on iOS where it is a no-op.
        virtual bool isWindowMaximized();
        virtual void maximizeWindow();
        virtual void restoreWindow();
        virtual void setWindowSize(int width, int height);
        virtual bool isWindowResizable();
        virtual void setWindowResizable(bool resizable);
        virtual void setWindowTitle(const std::string& title);
        virtual void quit();

        virtual char getDirSeparator();
        
        virtual std::string getAssetPath();
        virtual std::string getUserDataPath();
        virtual std::string getLuaPath();
        virtual std::string getShaderPath();
        // Where the editor keeps the models it imports; empty when each sits next to its source
        virtual std::string getImportPath();

        // *******
        // Used only for engine
        // *******
        virtual sg_environment getSokolEnvironment();
        virtual sg_swapchain getSokolSwapchain();

        virtual void setMouseCursor(CursorType type);
        virtual void setMouseMode(MouseMode mode);
        virtual void setMousePosition(float x, float y);

        virtual FILE* platformFopen(const char* fname, const char* mode);
        virtual bool syncFileSystem();

        virtual void platformLog(const int type, const char *fmt, va_list args);

        virtual bool getBoolForKey(const char *key, bool defaultValue);
        virtual int getIntegerForKey(const char *key, int defaultValue);
        virtual long getLongForKey(const char *key, long defaultValue);
        virtual float getFloatForKey(const char *key, float defaultValue);
        virtual double getDoubleForKey(const char *key, double defaultValue);
        virtual Data getDataForKey(const char *key, const Data& defaultValue);
        virtual std::string getStringForKey(const char *key, const std::string& defaultValue);

        virtual void setBoolForKey(const char *key, bool value);
        virtual void setIntegerForKey(const char *key, int value);
        virtual void setLongForKey(const char *key, long value);
        virtual void setFloatForKey(const char *key, float value);
        virtual void setDoubleForKey(const char *key, double value);
        virtual void setDataForKey(const char *key, Data& value);
        virtual void setStringForKey(const char* key, const std::string& value);

        virtual void removeKey(const char *key);

        // Services in core/service, null where the platform or the build has none
        virtual AdMobBackend* getAdMobBackend();
        virtual InAppPurchaseBackend* getInAppPurchaseBackend();
        virtual WebPortalBackend* getWebPortalBackend();

        // Whether this process's window is the one the desktop routes input to. A
        // backend that does not track it answers true, so a caller that gates on it
        // keeps the behaviour it had before the gate existed.
        // New virtuals go here, at the end: a game project compiles against a copy of
        // this header that only the editor refreshes, so a renumbered slot silently
        // turns an already-built binary's call into the wrong function.
        virtual bool hasFocus();
    };

}


#endif //SYSTEM_H
