// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "AdMob.h"

#include "Log.h"
#include "System.h"
#include <chrono>

using namespace quantum;

namespace {
    // Google stops serving app open ads loaded more than four hours ago
    const double appOpenAdLifetime = 4.0 * 3600.0;

    const char* notAvailableMessage = "AdMob is not available on this platform or build";

    int formatIndex(AdMobFormat format){
        return static_cast<int>(format);
    }

    AdMobBackend* getBackend(){
        return System::instance().getAdMobBackend();
    }

    // Wall clock, like Google's samples: ads keep aging while the device sleeps
    double wallTime(){
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void warnNotAvailable(){
        static bool warned = false;
        if (!warned){
            Log::warn("%s. Android and iOS exports have it when Google AdMob is enabled in the project settings.", notAvailableMessage);
            warned = true;
        }
    }
}

bool AdMob::initialized = false;
bool AdMob::loaded[AdMob::formatCount] = {};
double AdMob::loadTime[AdMob::formatCount] = {};
int AdMob::loadGenerations[AdMob::formatCount] = {};

bool AdMob::bannerVisible = false;
AdMobBannerPosition AdMob::bannerPosition = AdMobBannerPosition::BOTTOM;
int AdMob::bannerWidth = 0;
int AdMob::bannerHeight = 0;

AdMobRating AdMob::maxAdContentRating = AdMobRating::UNSPECIFIED;
AdMobAgeRestriction AdMob::ageRestriction = AdMobAgeRestriction::UNSPECIFIED;
AdMobPersonalization AdMob::personalization = AdMobPersonalization::DEFAULT;
AdMobDebugGeography AdMob::debugGeography = AdMobDebugGeography::DISABLED;
std::vector<std::string> AdMob::testDeviceIds;

std::mutex AdMob::eventMutex;
std::vector<std::function<void()>> AdMob::pendingEvents;

FunctionSubscribe<void()> AdMob::onInitialized;
FunctionSubscribe<void(int, std::string)> AdMob::onConsentUpdated;
FunctionSubscribe<void(AdMobFormat)> AdMob::onAdLoaded;
FunctionSubscribe<void(AdMobFormat, int, std::string)> AdMob::onAdFailedToLoad;
FunctionSubscribe<void(AdMobFormat)> AdMob::onAdShown;
FunctionSubscribe<void(AdMobFormat, int, std::string)> AdMob::onAdFailedToShow;
FunctionSubscribe<void(AdMobFormat)> AdMob::onAdDismissed;
FunctionSubscribe<void(AdMobFormat)> AdMob::onAdClicked;
FunctionSubscribe<void(AdMobFormat)> AdMob::onAdImpression;
FunctionSubscribe<void(AdMobFormat, std::string, int)> AdMob::onUserEarnedReward;
FunctionSubscribe<void(AdMobFormat, long long, std::string, AdMobPrecision)> AdMob::onAdPaid;
FunctionSubscribe<void(int, std::string)> AdMob::onAdInspectorClosed;

void AdMob::postEvent(std::function<void()> event){
    std::lock_guard<std::mutex> lock(eventMutex);
    pendingEvents.push_back(std::move(event));
}

void AdMob::dispatchEvents(){
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

void AdMob::clearSubscriptions(){
    onInitialized.clear();
    onConsentUpdated.clear();
    onAdLoaded.clear();
    onAdFailedToLoad.clear();
    onAdShown.clear();
    onAdFailedToShow.clear();
    onAdDismissed.clear();
    onAdClicked.clear();
    onAdImpression.clear();
    onUserEarnedReward.clear();
    onAdPaid.clear();
    onAdInspectorClosed.clear();
}

void AdMob::removeSubscriptionsByTag(const std::string& substring){
    onInitialized.removeByTagSubstring(substring);
    onConsentUpdated.removeByTagSubstring(substring);
    onAdLoaded.removeByTagSubstring(substring);
    onAdFailedToLoad.removeByTagSubstring(substring);
    onAdShown.removeByTagSubstring(substring);
    onAdFailedToShow.removeByTagSubstring(substring);
    onAdDismissed.removeByTagSubstring(substring);
    onAdClicked.removeByTagSubstring(substring);
    onAdImpression.removeByTagSubstring(substring);
    onUserEarnedReward.removeByTagSubstring(substring);
    onAdPaid.removeByTagSubstring(substring);
    onAdInspectorClosed.removeByTagSubstring(substring);
}

void AdMob::reset(){
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        pendingEvents.clear();
    }

    initialized = false;
    for (int i = 0; i < formatCount; i++){
        loaded[i] = false;
        loadTime[i] = 0;
        // a result still on its way must not match a later load
        loadGenerations[i]++;
    }

    bannerVisible = false;
    bannerPosition = AdMobBannerPosition::BOTTOM;
    bannerWidth = 0;
    bannerHeight = 0;

    maxAdContentRating = AdMobRating::UNSPECIFIED;
    ageRestriction = AdMobAgeRestriction::UNSPECIFIED;
    personalization = AdMobPersonalization::DEFAULT;
    debugGeography = AdMobDebugGeography::DISABLED;
    testDeviceIds.clear();
}

void AdMob::applyRequestConfiguration(){
    if (AdMobBackend* backend = getBackend()){
        backend->setRequestConfiguration(maxAdContentRating, ageRestriction, personalization, testDeviceIds);
    }
}

void AdMob::initialize(){
    // settings made before initialize reach the first requests
    applyRequestConfiguration();

    if (AdMobBackend* backend = getBackend()){
        backend->initialize();
    }else{
        warnNotAvailable();
    }
}

bool AdMob::isInitialized(){
    return initialized;
}

void AdMob::setMaxAdContentRating(AdMobRating rating){
    maxAdContentRating = rating;
    applyRequestConfiguration();
}

AdMobRating AdMob::getMaxAdContentRating(){
    return maxAdContentRating;
}

void AdMob::setAgeRestriction(AdMobAgeRestriction restriction){
    ageRestriction = restriction;
    applyRequestConfiguration();
}

AdMobAgeRestriction AdMob::getAgeRestriction(){
    return ageRestriction;
}

void AdMob::setPersonalization(AdMobPersonalization personalization){
    AdMob::personalization = personalization;
    applyRequestConfiguration();
}

AdMobPersonalization AdMob::getPersonalization(){
    return personalization;
}

void AdMob::setTestDeviceIds(const std::vector<std::string>& ids){
    testDeviceIds = ids;
    applyRequestConfiguration();
}

std::vector<std::string> AdMob::getTestDeviceIds(){
    return testDeviceIds;
}

void AdMob::requestConsent(){
    const bool underAgeOfConsent = (ageRestriction == AdMobAgeRestriction::CHILD);

    if (AdMobBackend* backend = getBackend()){
        backend->requestConsent(underAgeOfConsent, debugGeography, testDeviceIds);
    }else{
        warnNotAvailable();
        postEvent([](){ onConsentUpdated.call(errorNotAvailable, notAvailableMessage); });
    }
}

void AdMob::setConsentDebugGeography(AdMobDebugGeography geography){
    debugGeography = geography;
}

AdMobDebugGeography AdMob::getConsentDebugGeography(){
    return debugGeography;
}

AdMobConsentStatus AdMob::getConsentStatus(){
    AdMobBackend* backend = getBackend();
    return backend ? backend->getConsentStatus() : AdMobConsentStatus::UNKNOWN;
}

bool AdMob::canRequestAds(){
    AdMobBackend* backend = getBackend();
    return backend && backend->canRequestAds();
}

bool AdMob::isPrivacyOptionsRequired(){
    AdMobBackend* backend = getBackend();
    return backend && backend->isPrivacyOptionsRequired();
}

void AdMob::showPrivacyOptionsForm(){
    if (AdMobBackend* backend = getBackend()){
        backend->showPrivacyOptionsForm();
    }else{
        warnNotAvailable();
        postEvent([](){ onConsentUpdated.call(errorNotAvailable, notAvailableMessage); });
    }
}

void AdMob::resetConsent(){
    if (AdMobBackend* backend = getBackend()){
        backend->resetConsent();
    }
}

void AdMob::loadAd(AdMobFormat format, const std::string& adUnitId){
    const int i = formatIndex(format);
    loaded[i] = false;

    if (AdMobBackend* backend = getBackend()){
        backend->loadAd(format, adUnitId, ++loadGenerations[i]);
    }else{
        warnNotAvailable();
        postEvent([format](){ onAdFailedToLoad.call(format, errorNotAvailable, notAvailableMessage); });
    }
}

bool AdMob::isAdLoaded(AdMobFormat format){
    const int i = formatIndex(format);
    if (!loaded[i])
        return false;

    if (format == AdMobFormat::APP_OPEN && (wallTime() - loadTime[i]) > appOpenAdLifetime)
        return false;

    return true;
}

void AdMob::showAd(AdMobFormat format){
    if (!isAdLoaded(format)){
        const char* message = loaded[formatIndex(format)] ? "The ad expired, load it again" : "The ad is not loaded";
        postEvent([format, message](){ onAdFailedToShow.call(format, errorNotLoaded, message); });
        return;
    }

    // a full screen ad is shown only once
    loaded[formatIndex(format)] = false;

    if (AdMobBackend* backend = getBackend()){
        backend->showAd(format);
    }else{
        warnNotAvailable();
        postEvent([format](){ onAdFailedToShow.call(format, errorNotAvailable, notAvailableMessage); });
    }
}

void AdMob::loadBannerAd(const std::string& adUnitId, AdMobBannerSize size, AdMobBannerPosition position){
    const int i = formatIndex(AdMobFormat::BANNER);
    loaded[i] = false;
    bannerVisible = true;
    bannerPosition = position;
    bannerWidth = 0;
    bannerHeight = 0;

    if (AdMobBackend* backend = getBackend()){
        backend->loadBanner(adUnitId, size, position, bannerVisible, ++loadGenerations[i]);
    }else{
        warnNotAvailable();
        postEvent([](){ onAdFailedToLoad.call(AdMobFormat::BANNER, errorNotAvailable, notAvailableMessage); });
    }
}

bool AdMob::isBannerAdLoaded(){
    return loaded[formatIndex(AdMobFormat::BANNER)];
}

void AdMob::showBannerAd(){
    bannerVisible = true;
    if (AdMobBackend* backend = getBackend()){
        backend->setBannerVisible(true);
    }
}

void AdMob::hideBannerAd(){
    bannerVisible = false;
    if (AdMobBackend* backend = getBackend()){
        backend->setBannerVisible(false);
    }
}

bool AdMob::isBannerAdVisible(){
    return bannerVisible && isBannerAdLoaded();
}

void AdMob::setBannerAdPosition(AdMobBannerPosition position){
    bannerPosition = position;
    if (AdMobBackend* backend = getBackend()){
        backend->setBannerPosition(position);
    }
}

AdMobBannerPosition AdMob::getBannerAdPosition(){
    return bannerPosition;
}

void AdMob::removeBannerAd(){
    const int i = formatIndex(AdMobFormat::BANNER);
    loaded[i] = false;
    // results of the removed banner may still be queued
    loadGenerations[i]++;
    bannerVisible = false;
    bannerWidth = 0;
    bannerHeight = 0;

    if (AdMobBackend* backend = getBackend()){
        backend->removeBanner();
    }
}

int AdMob::getBannerAdWidth(){
    return bannerWidth;
}

int AdMob::getBannerAdHeight(){
    return bannerHeight;
}

void AdMob::loadInterstitialAd(const std::string& adUnitId){
    loadAd(AdMobFormat::INTERSTITIAL, adUnitId);
}

bool AdMob::isInterstitialAdLoaded(){
    return isAdLoaded(AdMobFormat::INTERSTITIAL);
}

void AdMob::showInterstitialAd(){
    showAd(AdMobFormat::INTERSTITIAL);
}

void AdMob::loadRewardedAd(const std::string& adUnitId){
    loadAd(AdMobFormat::REWARDED, adUnitId);
}

bool AdMob::isRewardedAdLoaded(){
    return isAdLoaded(AdMobFormat::REWARDED);
}

void AdMob::showRewardedAd(){
    showAd(AdMobFormat::REWARDED);
}

void AdMob::loadRewardedInterstitialAd(const std::string& adUnitId){
    loadAd(AdMobFormat::REWARDED_INTERSTITIAL, adUnitId);
}

bool AdMob::isRewardedInterstitialAdLoaded(){
    return isAdLoaded(AdMobFormat::REWARDED_INTERSTITIAL);
}

void AdMob::showRewardedInterstitialAd(){
    showAd(AdMobFormat::REWARDED_INTERSTITIAL);
}

void AdMob::loadAppOpenAd(const std::string& adUnitId){
    loadAd(AdMobFormat::APP_OPEN, adUnitId);
}

bool AdMob::isAppOpenAdLoaded(){
    return isAdLoaded(AdMobFormat::APP_OPEN);
}

void AdMob::showAppOpenAd(){
    showAd(AdMobFormat::APP_OPEN);
}

void AdMob::setServerSideVerificationOptions(const std::string& userId, const std::string& customData){
    if (AdMobBackend* backend = getBackend()){
        backend->setServerSideVerificationOptions(userId, customData);
    }
}

void AdMob::setAppVolume(float volume){
    if (AdMobBackend* backend = getBackend()){
        backend->setAppVolume(volume);
    }
}

void AdMob::setAppMuted(bool muted){
    if (AdMobBackend* backend = getBackend()){
        backend->setAppMuted(muted);
    }
}

void AdMob::openAdInspector(){
    if (AdMobBackend* backend = getBackend()){
        backend->openAdInspector();
    }else{
        warnNotAvailable();
        postEvent([](){ onAdInspectorClosed.call(errorNotAvailable, notAvailableMessage); });
    }
}

void AdMob::systemInitialized(){
    postEvent([](){
        initialized = true;
        onInitialized.call();
    });
}

void AdMob::systemConsentUpdated(int errorCode, const std::string& message){
    postEvent([errorCode, message](){ onConsentUpdated.call(errorCode, message); });
}

void AdMob::systemAdLoaded(AdMobFormat format, int generation, int width, int height){
    // timed here, not on dispatch: a load that ends in background waits for the next frame
    const double time = wallTime();
    postEvent([format, generation, width, height, time](){
        const int i = formatIndex(format);
        // the ad was replaced or removed while this was on its way
        if (generation != loadGenerations[i])
            return;

        loaded[i] = true;
        loadTime[i] = time;
        if (format == AdMobFormat::BANNER){
            bannerWidth = width;
            bannerHeight = height;
        }
        onAdLoaded.call(format);
    });
}

void AdMob::systemAdFailedToLoad(AdMobFormat format, int generation, int errorCode, const std::string& message){
    postEvent([format, generation, errorCode, message](){
        const int i = formatIndex(format);
        if (generation != loadGenerations[i])
            return;

        // a banner that fails to refresh keeps showing its last ad
        if (format != AdMobFormat::BANNER)
            loaded[i] = false;
        onAdFailedToLoad.call(format, errorCode, message);
    });
}

void AdMob::systemAdShown(AdMobFormat format){
    postEvent([format](){ onAdShown.call(format); });
}

void AdMob::systemAdFailedToShow(AdMobFormat format, int errorCode, const std::string& message){
    postEvent([format, errorCode, message](){ onAdFailedToShow.call(format, errorCode, message); });
}

void AdMob::systemAdDismissed(AdMobFormat format){
    postEvent([format](){ onAdDismissed.call(format); });
}

void AdMob::systemAdClicked(AdMobFormat format){
    postEvent([format](){ onAdClicked.call(format); });
}

void AdMob::systemAdImpression(AdMobFormat format){
    postEvent([format](){ onAdImpression.call(format); });
}

void AdMob::systemUserEarnedReward(AdMobFormat format, const std::string& type, int amount){
    postEvent([format, type, amount](){ onUserEarnedReward.call(format, type, amount); });
}

void AdMob::systemAdPaid(AdMobFormat format, long long valueMicros, const std::string& currencyCode, AdMobPrecision precision){
    postEvent([format, valueMicros, currencyCode, precision](){ onAdPaid.call(format, valueMicros, currencyCode, precision); });
}

void AdMob::systemAdInspectorClosed(int errorCode, const std::string& message){
    postEvent([errorCode, message](){ onAdInspectorClosed.call(errorCode, message); });
}
