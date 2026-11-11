// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef ADMOB_H
#define ADMOB_H

#include "Export.h"
#include "util/FunctionSubscribe.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace quantum {

    // Sent to the platforms as integers, so keep the order
    enum class AdMobFormat{
        BANNER,
        INTERSTITIAL,
        REWARDED,
        REWARDED_INTERSTITIAL,
        APP_OPEN
    };

    enum class AdMobBannerSize{
        ADAPTIVE,         // anchored adaptive: screen width, height chosen by the SDK
        BANNER,           // 320x50
        LARGE_BANNER,     // 320x100
        MEDIUM_RECTANGLE, // 300x250
        FULL_BANNER,      // 468x60
        LEADERBOARD       // 728x90
    };

    enum class AdMobBannerPosition{
        TOP,
        BOTTOM,
        TOP_LEFT,
        TOP_RIGHT,
        BOTTOM_LEFT,
        BOTTOM_RIGHT,
        CENTER
    };

    enum class AdMobRating{
        UNSPECIFIED,
        GENERAL,
        PARENTAL_GUIDANCE,
        TEEN,
        MATURE_AUDIENCE
    };

    // Replaces Google's child-directed and under-age-of-consent tags
    enum class AdMobAgeRestriction{
        UNSPECIFIED,
        CHILD,
        TEEN
    };

    enum class AdMobPersonalization{
        DEFAULT,
        ENABLED,
        DISABLED
    };

    enum class AdMobConsentStatus{
        UNKNOWN,
        REQUIRED,
        NOT_REQUIRED,
        OBTAINED
    };

    // Simulated user location for consent testing, only on test devices
    enum class AdMobDebugGeography{
        DISABLED,
        EEA,
        REGULATED_US_STATE,
        OTHER
    };

    enum class AdMobPrecision{
        UNKNOWN,
        ESTIMATED,
        PUBLISHER_PROVIDED,
        PRECISE
    };

    // What a platform with Google Mobile Ads implements, see System::getAdMobBackend.
    // Called from the engine thread, it answers through the AdMob::system* callbacks.
    class QUANTUM_API AdMobBackend {
    public:
        virtual ~AdMobBackend() = default;

        virtual void initialize() = 0;
        virtual void setRequestConfiguration(AdMobRating rating, AdMobAgeRestriction ageRestriction, AdMobPersonalization personalization, const std::vector<std::string>& testDeviceIds) = 0;
        virtual void requestConsent(bool underAgeOfConsent, AdMobDebugGeography debugGeography, const std::vector<std::string>& testDeviceIds) = 0;
        virtual AdMobConsentStatus getConsentStatus() = 0;
        virtual bool canRequestAds() = 0;
        virtual bool isPrivacyOptionsRequired() = 0;
        virtual void showPrivacyOptionsForm() = 0;
        virtual void resetConsent() = 0;
        // generation goes back with the load result
        virtual void loadAd(AdMobFormat format, const std::string& adUnitId, int generation) = 0;
        virtual void showAd(AdMobFormat format) = 0;
        virtual void loadBanner(const std::string& adUnitId, AdMobBannerSize size, AdMobBannerPosition position, bool visible, int generation) = 0;
        virtual void setBannerVisible(bool visible) = 0;
        virtual void setBannerPosition(AdMobBannerPosition position) = 0;
        virtual void removeBanner() = 0;
        virtual void setServerSideVerificationOptions(const std::string& userId, const std::string& customData) = 0;
        virtual void setAppVolume(float volume) = 0;
        virtual void setAppMuted(bool muted) = 0;
        virtual void openAdInspector() = 0;
    };

    // Google Mobile Ads on Android and iOS. Results arrive as events at the start of a frame,
    // and loading a format again replaces its ad.
    class QUANTUM_API AdMob {

        friend class Engine;

    private:
        static const int formatCount = 5;

        static bool initialized;
        static bool loaded[formatCount];
        static double loadTime[formatCount];
        // bumped by each load and banner removal to drop older results
        static int loadGenerations[formatCount];

        static bool bannerVisible;
        static AdMobBannerPosition bannerPosition;
        static int bannerWidth;
        static int bannerHeight;

        static AdMobRating maxAdContentRating;
        static AdMobAgeRestriction ageRestriction;
        static AdMobPersonalization personalization;
        static AdMobDebugGeography debugGeography;
        static std::vector<std::string> testDeviceIds;

        static std::mutex eventMutex;
        static std::vector<std::function<void()>> pendingEvents;

        static void postEvent(std::function<void()> event);
        static void applyRequestConfiguration();

        static void loadAd(AdMobFormat format, const std::string& adUnitId);
        static bool isAdLoaded(AdMobFormat format);
        static void showAd(AdMobFormat format);

        // Called by Engine
        static void dispatchEvents();
        static void clearSubscriptions();
        static void removeSubscriptionsByTag(const std::string& substring);
        static void reset();

    public:
        // Errors reported by the engine instead of the Google SDK
        static const int errorNotAvailable = -1;
        static const int errorNotLoaded = -2;

        static void initialize();
        static bool isInitialized();

        // Used by the next ad requests
        static void setMaxAdContentRating(AdMobRating rating);
        static AdMobRating getMaxAdContentRating();
        static void setAgeRestriction(AdMobAgeRestriction restriction);
        static AdMobAgeRestriction getAgeRestriction();
        static void setPersonalization(AdMobPersonalization personalization);
        static AdMobPersonalization getPersonalization();
        // Hashed ids the SDKs print to the device log; these devices get test ads
        static void setTestDeviceIds(const std::vector<std::string>& ids);
        static std::vector<std::string> getTestDeviceIds();

        // Shows the consent form when needed; CHILD age restriction asks as under the age of consent
        static void requestConsent();
        static void setConsentDebugGeography(AdMobDebugGeography geography);
        static AdMobDebugGeography getConsentDebugGeography();
        static AdMobConsentStatus getConsentStatus();
        static bool canRequestAds();
        // A game must offer a way to reopen the form when this is true
        static bool isPrivacyOptionsRequired();
        static void showPrivacyOptionsForm();
        static void resetConsent();

        // An adaptive banner takes the screen width when loaded; load it again after rotation
        static void loadBannerAd(const std::string& adUnitId, AdMobBannerSize size = AdMobBannerSize::ADAPTIVE, AdMobBannerPosition position = AdMobBannerPosition::BOTTOM);
        static bool isBannerAdLoaded();
        static void showBannerAd();
        static void hideBannerAd();
        static bool isBannerAdVisible();
        static void setBannerAdPosition(AdMobBannerPosition position);
        static AdMobBannerPosition getBannerAdPosition();
        static void removeBannerAd();
        // Pixels, zero until the banner loads
        static int getBannerAdWidth();
        static int getBannerAdHeight();

        // Full screen ads can be shown once; load again after showing
        static void loadInterstitialAd(const std::string& adUnitId);
        static bool isInterstitialAdLoaded();
        static void showInterstitialAd();

        static void loadRewardedAd(const std::string& adUnitId);
        static bool isRewardedAdLoaded();
        static void showRewardedAd();

        static void loadRewardedInterstitialAd(const std::string& adUnitId);
        static bool isRewardedInterstitialAdLoaded();
        static void showRewardedInterstitialAd();

        // App open ads expire four hours after loading
        static void loadAppOpenAd(const std::string& adUnitId);
        static bool isAppOpenAdLoaded();
        static void showAppOpenAd();

        // Sent with rewards to the server-side verification callback
        static void setServerSideVerificationOptions(const std::string& userId, const std::string& customData);

        // Volume of video ads, relative to the game
        static void setAppVolume(float volume);
        static void setAppMuted(bool muted);

        static void openAdInspector();

        static FunctionSubscribe<void()> onInitialized;
        // errorCode is 0 on success
        static FunctionSubscribe<void(int, std::string)> onConsentUpdated;
        static FunctionSubscribe<void(AdMobFormat)> onAdLoaded;
        static FunctionSubscribe<void(AdMobFormat, int, std::string)> onAdFailedToLoad;
        // Full screen content opened; for banners, an overlay opened by a click
        static FunctionSubscribe<void(AdMobFormat)> onAdShown;
        static FunctionSubscribe<void(AdMobFormat, int, std::string)> onAdFailedToShow;
        static FunctionSubscribe<void(AdMobFormat)> onAdDismissed;
        static FunctionSubscribe<void(AdMobFormat)> onAdClicked;
        static FunctionSubscribe<void(AdMobFormat)> onAdImpression;
        // reward type and amount, as configured on the ad unit
        static FunctionSubscribe<void(AdMobFormat, std::string, int)> onUserEarnedReward;
        // estimated revenue in micros of the currency
        static FunctionSubscribe<void(AdMobFormat, long long, std::string, AdMobPrecision)> onAdPaid;
        static FunctionSubscribe<void(int, std::string)> onAdInspectorClosed;

        // Platform callbacks, safe from any thread
        static void systemInitialized();
        static void systemConsentUpdated(int errorCode, const std::string& message);
        static void systemAdLoaded(AdMobFormat format, int generation, int width, int height);
        static void systemAdFailedToLoad(AdMobFormat format, int generation, int errorCode, const std::string& message);
        static void systemAdShown(AdMobFormat format);
        static void systemAdFailedToShow(AdMobFormat format, int errorCode, const std::string& message);
        static void systemAdDismissed(AdMobFormat format);
        static void systemAdClicked(AdMobFormat format);
        static void systemAdImpression(AdMobFormat format);
        static void systemUserEarnedReward(AdMobFormat format, const std::string& type, int amount);
        static void systemAdPaid(AdMobFormat format, long long valueMicros, const std::string& currencyCode, AdMobPrecision precision);
        static void systemAdInspectorClosed(int errorCode, const std::string& message);
    };

}

#endif //ADMOB_H
