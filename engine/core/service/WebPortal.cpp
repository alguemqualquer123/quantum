// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "WebPortal.h"

#include "Log.h"
#include "System.h"

using namespace quantum;

namespace {
    const char* notAvailableMessage = "No game portal SDK in this build";

    void warnNotAvailable(){
        static bool warned = false;
        if (!warned){
            Log::warn("%s. Web exports have it when a portal is chosen in the project settings.", notAvailableMessage);
            warned = true;
        }
    }

    WebPortalBackend* getBackend(){
        return System::instance().getWebPortalBackend();
    }
}

WebPortalEnvironment WebPortal::environment = WebPortalEnvironment::UNINITIALIZED;

std::mutex WebPortal::eventMutex;
std::vector<std::function<void()>> WebPortal::pendingEvents;

FunctionSubscribe<void(WebPortalEnvironment)> WebPortal::onInitialized;
FunctionSubscribe<void(WebPortalAdType)> WebPortal::onAdStarted;
FunctionSubscribe<void(WebPortalAdType)> WebPortal::onAdFinished;
FunctionSubscribe<void(WebPortalAdType, std::string, std::string)> WebPortal::onAdError;
FunctionSubscribe<void(std::string)> WebPortal::onDataLoaded;
FunctionSubscribe<void(std::string)> WebPortal::onDataLoadFailed;
FunctionSubscribe<void(std::string)> WebPortal::onDataSaveFailed;

void WebPortal::postEvent(std::function<void()> event){
    std::lock_guard<std::mutex> lock(eventMutex);
    pendingEvents.push_back(std::move(event));
}

void WebPortal::dispatchEvents(){
    std::vector<std::function<void()>> events;
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        events.swap(pendingEvents);
    }
    // events posted by these callbacks wait for the next frame
    for (auto& event : events){
        event();
    }
}

void WebPortal::clearSubscriptions(){
    onInitialized.clear();
    onAdStarted.clear();
    onAdFinished.clear();
    onAdError.clear();
    onDataLoaded.clear();
    onDataLoadFailed.clear();
    onDataSaveFailed.clear();
}

void WebPortal::removeSubscriptionsByTag(const std::string& substring){
    onInitialized.removeByTagSubstring(substring);
    onAdStarted.removeByTagSubstring(substring);
    onAdFinished.removeByTagSubstring(substring);
    onAdError.removeByTagSubstring(substring);
    onDataLoaded.removeByTagSubstring(substring);
    onDataLoadFailed.removeByTagSubstring(substring);
    onDataSaveFailed.removeByTagSubstring(substring);
}

void WebPortal::reset(){
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        pendingEvents.clear();
    }

    environment = WebPortalEnvironment::UNINITIALIZED;
}

WebPortalType WebPortal::getPortal(){
    WebPortalBackend* backend = getBackend();
    return backend ? backend->getType() : WebPortalType::NONE;
}

void WebPortal::initialize(){
    if (WebPortalBackend* backend = getBackend()){
        backend->initialize();
    }else{
        warnNotAvailable();
        postEvent([](){
            environment = WebPortalEnvironment::DISABLED;
            onInitialized.call(environment);
        });
    }
}

WebPortalEnvironment WebPortal::getEnvironment(){
    return environment;
}

bool WebPortal::isAvailable(){
    return environment == WebPortalEnvironment::LOCAL || environment == WebPortalEnvironment::PORTAL;
}

void WebPortal::requestAd(WebPortalAdType type){
    if (WebPortalBackend* backend = getBackend()){
        backend->requestAd(type);
    }else{
        warnNotAvailable();
        postEvent([type](){ onAdError.call(type, "unavailable", notAvailableMessage); });
    }
}

void WebPortal::gameplayStart(){
    if (WebPortalBackend* backend = getBackend()){
        backend->gameplayStart();
    }
}

void WebPortal::gameplayStop(){
    if (WebPortalBackend* backend = getBackend()){
        backend->gameplayStop();
    }
}

void WebPortal::loadingStart(){
    if (WebPortalBackend* backend = getBackend()){
        backend->loadingStart();
    }
}

void WebPortal::loadingStop(){
    if (WebPortalBackend* backend = getBackend()){
        backend->loadingStop();
    }
}

void WebPortal::happytime(){
    if (WebPortalBackend* backend = getBackend()){
        backend->happytime();
    }
}

void WebPortal::loadData(){
    if (WebPortalBackend* backend = getBackend()){
        backend->loadData();
    }else{
        warnNotAvailable();
        postEvent([](){ onDataLoadFailed.call(notAvailableMessage); });
    }
}

void WebPortal::saveData(const std::string& data){
    if (WebPortalBackend* backend = getBackend()){
        backend->saveData(data);
    }else{
        warnNotAvailable();
        postEvent([](){ onDataSaveFailed.call(notAvailableMessage); });
    }
}

void WebPortal::systemInitialized(WebPortalEnvironment environment, const std::string& error){
    if (!error.empty()){
        Log::warn("Game portal SDK: %s", error.c_str());
    }
    postEvent([environment](){
        WebPortal::environment = environment;
        onInitialized.call(environment);
    });
}

void WebPortal::systemAdStarted(WebPortalAdType type){
    postEvent([type](){ onAdStarted.call(type); });
}

void WebPortal::systemAdFinished(WebPortalAdType type){
    postEvent([type](){ onAdFinished.call(type); });
}

void WebPortal::systemAdError(WebPortalAdType type, const std::string& code, const std::string& message){
    postEvent([type, code, message](){ onAdError.call(type, code, message); });
}

void WebPortal::systemDataLoaded(const std::string& data){
    postEvent([data](){ onDataLoaded.call(data); });
}

void WebPortal::systemDataLoadFailed(const std::string& message){
    postEvent([message](){ onDataLoadFailed.call(message); });
}

void WebPortal::systemDataSaveFailed(const std::string& message){
    postEvent([message](){ onDataSaveFailed.call(message); });
}
