// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QuantumApple_h
#define QuantumApple_h

#include "System.h"

class QuantumApple: public quantum::System{

public:

    QuantumApple();

    virtual ~QuantumApple();

    virtual sg_environment getSokolEnvironment() override;
    virtual sg_swapchain getSokolSwapchain() override;

    virtual void setMouseCursor(quantum::CursorType type) override;
    virtual void setMouseMode(quantum::MouseMode mode) override;

    virtual int getScreenWidth() override;
    virtual int getScreenHeight() override;

    virtual int getSampleCount() override;

    virtual bool isTouchDevice() override;

    virtual void showVirtualKeyboard(std::wstring text) override;
    virtual void hideVirtualKeyboard() override;

    virtual void quit() override;

    virtual std::string getAssetPath() override;
    virtual std::string getUserDataPath() override;
    virtual std::string getLuaPath() override;
    virtual std::string getShaderPath() override;

    virtual bool getBoolForKey(const char *key, bool defaultValue) override;
    virtual int getIntegerForKey(const char *key, int defaultValue) override;
    virtual long getLongForKey(const char *key, long defaultValue) override;
    virtual float getFloatForKey(const char *key, float defaultValue) override;
    virtual double getDoubleForKey(const char *key, double defaultValue) override;
    virtual quantum::Data getDataForKey(const char* key, const quantum::Data& defaultValue) override;
    virtual std::string getStringForKey(const char *key, const std::string& defaultValue) override;

    virtual void setBoolForKey(const char *key, bool value) override;
    virtual void setIntegerForKey(const char *key, int value) override;
    virtual void setLongForKey(const char *key, long value) override;
    virtual void setFloatForKey(const char *key, float value) override;
    virtual void setDoubleForKey(const char *key, double value) override;
    virtual void setDataForKey(const char* key, quantum::Data& value) override;
    virtual void setStringForKey(const char* key, const std::string& value) override;

    virtual void removeKey(const char *key) override;

    // iOS builds with QUANTUM_ADMOB only
    virtual quantum::AdMobBackend* getAdMobBackend() override;
    // iOS builds with QUANTUM_STOREKIT only
    virtual quantum::InAppPurchaseBackend* getInAppPurchaseBackend() override;
};


#endif /* QuantumApple_h */
