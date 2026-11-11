// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "QuantumApple.h"

#include "Engine.h"
#include "Log.h"
#include "service/AdMob.h"
#include "service/InAppPurchase.h"

#import "Renderer.h"
#import <Foundation/Foundation.h>

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#endif

#if TARGET_OS_IPHONE
#import <MetalKit/MetalKit.h>
#endif

// Google Mobile Ads is linked into iOS builds that keep AdMob
#if TARGET_OS_IPHONE && defined(QUANTUM_ADMOB)
#define QUANTUM_IOS_ADMOB
#import "ios/AdMobAdapter.h"
#endif

// The StoreKit adapter is compiled into iOS builds that keep App Store purchases
#if TARGET_OS_IPHONE && defined(QUANTUM_STOREKIT)
#define QUANTUM_IOS_STOREKIT
#import "ios/StoreKitAdapter.h"
#endif

#if defined(QUANTUM_IOS_ADMOB) || defined(QUANTUM_IOS_STOREKIT)
static NSString* toNSString(const std::string& text){
    NSString* value = [NSString stringWithUTF8String:text.c_str()];
    return value ? value : @"";
}

static NSArray<NSString*>* toNSArray(const std::vector<std::string>& values){
    NSMutableArray<NSString*>* array = [NSMutableArray arrayWithCapacity:values.size()];
    for (const std::string& value : values){
        [array addObject:toNSString(value)];
    }
    return array;
}
#endif

#ifdef QUANTUM_IOS_ADMOB
static AdMobAdapter* admob = nil;

extern "C" {
void QuantumAdMobInitialized(void){
    quantum::AdMob::systemInitialized();
}

void QuantumAdMobConsentUpdated(int errorCode, const char* message){
    quantum::AdMob::systemConsentUpdated(errorCode, message);
}

void QuantumAdMobAdLoaded(int format, int generation, int width, int height){
    quantum::AdMob::systemAdLoaded(static_cast<quantum::AdMobFormat>(format), generation, width, height);
}

void QuantumAdMobAdFailedToLoad(int format, int generation, int errorCode, const char* message){
    quantum::AdMob::systemAdFailedToLoad(static_cast<quantum::AdMobFormat>(format), generation, errorCode, message);
}

void QuantumAdMobAdShown(int format){
    quantum::AdMob::systemAdShown(static_cast<quantum::AdMobFormat>(format));
}

void QuantumAdMobAdFailedToShow(int format, int errorCode, const char* message){
    quantum::AdMob::systemAdFailedToShow(static_cast<quantum::AdMobFormat>(format), errorCode, message);
}

void QuantumAdMobAdDismissed(int format){
    quantum::AdMob::systemAdDismissed(static_cast<quantum::AdMobFormat>(format));
}

void QuantumAdMobAdClicked(int format){
    quantum::AdMob::systemAdClicked(static_cast<quantum::AdMobFormat>(format));
}

void QuantumAdMobAdImpression(int format){
    quantum::AdMob::systemAdImpression(static_cast<quantum::AdMobFormat>(format));
}

void QuantumAdMobAdPaid(int format, long long valueMicros, const char* currencyCode, int precision){
    quantum::AdMob::systemAdPaid(static_cast<quantum::AdMobFormat>(format), valueMicros, currencyCode, static_cast<quantum::AdMobPrecision>(precision));
}

void QuantumAdMobUserEarnedReward(int format, const char* type, int amount){
    quantum::AdMob::systemUserEarnedReward(static_cast<quantum::AdMobFormat>(format), type, amount);
}

void QuantumAdMobAdInspectorClosed(int errorCode, const char* message){
    quantum::AdMob::systemAdInspectorClosed(errorCode, message);
}
}

namespace {
    // Google Mobile Ads through AdMobAdapter
    class AppleAdMob: public quantum::AdMobBackend{
    public:
        virtual void initialize() override{
            [admob initializeAdMob];
        }

        virtual void setRequestConfiguration(quantum::AdMobRating rating, quantum::AdMobAgeRestriction ageRestriction, quantum::AdMobPersonalization personalization, const std::vector<std::string>& testDeviceIds) override{
            [admob setRequestConfigurationWithRating:static_cast<int>(rating) ageRestriction:static_cast<int>(ageRestriction)
                personalization:static_cast<int>(personalization) testDeviceIds:toNSArray(testDeviceIds)];
        }

        virtual void requestConsent(bool underAgeOfConsent, quantum::AdMobDebugGeography debugGeography, const std::vector<std::string>& testDeviceIds) override{
            [admob requestConsentUnderAge:underAgeOfConsent debugGeography:static_cast<int>(debugGeography) testDeviceIds:toNSArray(testDeviceIds)];
        }

        virtual quantum::AdMobConsentStatus getConsentStatus() override{
            return static_cast<quantum::AdMobConsentStatus>([admob consentStatus]);
        }

        virtual bool canRequestAds() override{
            return [admob canRequestAds];
        }

        virtual bool isPrivacyOptionsRequired() override{
            return [admob isPrivacyOptionsRequired];
        }

        virtual void showPrivacyOptionsForm() override{
            [admob showPrivacyOptionsForm];
        }

        virtual void resetConsent() override{
            [admob resetConsent];
        }

        virtual void loadAd(quantum::AdMobFormat format, const std::string& adUnitId, int generation) override{
            [admob loadAd:static_cast<int>(format) adUnitId:toNSString(adUnitId) generation:generation];
        }

        virtual void showAd(quantum::AdMobFormat format) override{
            [admob showAd:static_cast<int>(format)];
        }

        virtual void loadBanner(const std::string& adUnitId, quantum::AdMobBannerSize size, quantum::AdMobBannerPosition position, bool visible, int generation) override{
            [admob loadBanner:toNSString(adUnitId) size:static_cast<int>(size) position:static_cast<int>(position) visible:visible generation:generation];
        }

        virtual void setBannerVisible(bool visible) override{
            [admob setBannerVisible:visible];
        }

        virtual void setBannerPosition(quantum::AdMobBannerPosition position) override{
            [admob setBannerPosition:static_cast<int>(position)];
        }

        virtual void removeBanner() override{
            [admob removeBanner];
        }

        virtual void setServerSideVerificationOptions(const std::string& userId, const std::string& customData) override{
            [admob setServerSideVerificationUserId:toNSString(userId) customData:toNSString(customData)];
        }

        virtual void setAppVolume(float volume) override{
            [admob setAppVolume:volume];
        }

        virtual void setAppMuted(bool muted) override{
            [admob setAppMuted:muted];
        }

        virtual void openAdInspector() override{
            [admob openAdInspector];
        }
    };

    AppleAdMob appleAdMob;
}
#endif

#ifdef QUANTUM_IOS_STOREKIT
static StoreKitAdapter* storeKit = nil;

extern "C" {
void QuantumStoreKitInitialized(int responseCode, const char* message){
    quantum::InAppPurchase::systemInitialized(static_cast<quantum::BillingResponse>(responseCode), message);
}

void QuantumStoreKitProductsQueried(int responseCode, const char* message, const char* productsJson){
    quantum::InAppPurchase::systemProductsQueried(static_cast<quantum::BillingResponse>(responseCode), message,
        quantum::InAppPurchase::productsFromJson(productsJson));
}

void QuantumStoreKitPurchaseUpdated(const char* purchaseJson){
    for (const quantum::PurchaseDetails& purchase : quantum::InAppPurchase::purchasesFromJson(purchaseJson)){
        quantum::InAppPurchase::systemPurchaseUpdated(purchase);
    }
}

void QuantumStoreKitPurchaseFailed(const char* productId, int responseCode, const char* message){
    quantum::InAppPurchase::systemPurchaseFailed(productId, static_cast<quantum::BillingResponse>(responseCode), message);
}

void QuantumStoreKitPurchasesQueried(int productType, int responseCode, const char* message, const char* purchasesJson){
    quantum::InAppPurchase::systemPurchasesQueried(static_cast<quantum::ProductType>(productType), static_cast<quantum::BillingResponse>(responseCode), message,
        quantum::InAppPurchase::purchasesFromJson(purchasesJson));
}

void QuantumStoreKitPurchaseAcknowledged(const char* purchaseToken, int responseCode, const char* message){
    quantum::InAppPurchase::systemPurchaseAcknowledged(purchaseToken, static_cast<quantum::BillingResponse>(responseCode), message);
}

void QuantumStoreKitPurchaseConsumed(const char* purchaseToken, int responseCode, const char* message){
    quantum::InAppPurchase::systemPurchaseConsumed(purchaseToken, static_cast<quantum::BillingResponse>(responseCode), message);
}
}

namespace {
    // StoreKit 2 through StoreKitAdapter
    class AppleInAppPurchase: public quantum::InAppPurchaseBackend{
    public:
        virtual void initialize() override{
            [storeKit initializeStore];
        }

        virtual bool isReady() override{
            return [storeKit isReady];
        }

        virtual void queryProducts(const std::vector<std::string>& productIds, quantum::ProductType type) override{
            [storeKit queryProducts:toNSArray(productIds) type:static_cast<int>(type)];
        }

        // The App Store has no offer tokens or replacement modes
        virtual void purchase(const quantum::PurchaseParams& params) override{
            NSString* accountToken = toNSString(params.obfuscatedAccountId);
            if (accountToken.length > 0 && ![[NSUUID alloc] initWithUUIDString:accountToken]){
                quantum::Log::warn("The App Store takes the obfuscated account id only as a UUID, so it is left out");
                accountToken = @"";
            }
            [storeKit purchase:toNSString(params.productId) accountToken:accountToken];
        }

        virtual void acknowledgePurchase(const std::string& purchaseToken) override{
            [storeKit finishPurchase:toNSString(purchaseToken) consume:NO];
        }

        virtual void consumePurchase(const std::string& purchaseToken) override{
            [storeKit finishPurchase:toNSString(purchaseToken) consume:YES];
        }

        virtual void queryPurchases(quantum::ProductType type) override{
            [storeKit queryPurchases:static_cast<int>(type)];
        }

        virtual void restorePurchases() override{
            [storeKit restorePurchases];
        }

        virtual void openSubscriptionManagement(const std::string& productId) override{
            [storeKit openSubscriptionManagement:toNSString(productId)];
        }

        virtual void showInAppMessages() override{
            // StoreKit shows them by itself
        }
    };

    AppleInAppPurchase appleInAppPurchase;
}
#endif

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
static bool appleMouseLocked = false;
static bool appleCursorHidden = false;

// [NSCursor hide]/[NSCursor unhide] are reference-counted and not idempotent,
// so track the net state and only toggle on an actual transition.
static void setAppleCursorHidden(bool hidden){
    if (hidden == appleCursorHidden)
        return;
    if (hidden){
        [NSCursor hide];
    }else{
        [NSCursor unhide];
    }
    appleCursorHidden = hidden;
}
#endif

QuantumApple::QuantumApple(){
#ifdef QUANTUM_IOS_ADMOB
    if (!admob)
        admob = [[AdMobAdapter alloc]init];
#endif
#ifdef QUANTUM_IOS_STOREKIT
    if (!storeKit)
        storeKit = [[StoreKitAdapter alloc]init];
#endif
}

QuantumApple::~QuantumApple(){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    CGAssociateMouseAndMouseCursorPosition(true);
    appleMouseLocked = false;
    setAppleCursorHidden(false);
#endif
#ifdef QUANTUM_IOS_ADMOB
    admob = nil;
#endif
#ifdef QUANTUM_IOS_STOREKIT
    storeKit = nil;
#endif
}

sg_environment QuantumApple::getSokolEnvironment(){
    return (sg_environment) {
        .defaults = {
            .color_format = SG_PIXELFORMAT_BGRA8,
            .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
            .sample_count = (int)Renderer.view.sampleCount,
        },
        .metal = {
            .device = (__bridge const void*) Renderer.view.device,
        }
    };
}

sg_swapchain QuantumApple::getSokolSwapchain(){
    return (sg_swapchain) {
        .width = (int)Renderer.screenSize.width,
        .height = (int)Renderer.screenSize.height,
        .sample_count = (int)Renderer.view.sampleCount,
        .color_format = SG_PIXELFORMAT_BGRA8,
        .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
        .metal = {
            .current_drawable = (__bridge const void*) [Renderer.view currentDrawable],
            .depth_stencil_texture = (__bridge const void*) [Renderer.view depthStencilTexture],
            .msaa_color_texture = (__bridge const void*) [Renderer.view multisampleColorTexture],
        }
    };
}

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
// undocumented methods for creating cursors (see GLFW 3.4 and imgui_impl_osx.mm)
@interface NSCursor()
+ (id)_windowResizeNorthWestSouthEastCursor;
+ (id)_windowResizeNorthEastSouthWestCursor;
+ (id)_windowResizeNorthSouthCursor;
+ (id)_windowResizeEastWestCursor;
@end
#endif

void QuantumApple::setMouseCursor(quantum::CursorType type){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    NSCursor* cursor;
    if (type == quantum::CursorType::ARROW){
        cursor = [NSCursor arrowCursor];
    }else if (type == quantum::CursorType::IBEAM){
        cursor = [NSCursor IBeamCursor];
    }else if (type == quantum::CursorType::CROSSHAIR){
        cursor = [NSCursor crosshairCursor];
    }else if (type == quantum::CursorType::POINTING_HAND){
        cursor = [NSCursor pointingHandCursor];
    }else if (type == quantum::CursorType::RESIZE_EW){
        cursor = [NSCursor respondsToSelector:@selector(_windowResizeEastWestCursor)] ? [NSCursor _windowResizeEastWestCursor] : [NSCursor resizeLeftRightCursor];
    }else if (type == quantum::CursorType::RESIZE_NS){
        cursor = [NSCursor respondsToSelector:@selector(_windowResizeNorthSouthCursor)] ? [NSCursor _windowResizeNorthSouthCursor] : [NSCursor resizeUpDownCursor];
    }else if (type == quantum::CursorType::RESIZE_NWSE){
        cursor = [NSCursor respondsToSelector:@selector(_windowResizeNorthWestSouthEastCursor)] ? [NSCursor _windowResizeNorthWestSouthEastCursor] : [NSCursor closedHandCursor];
    }else if (type == quantum::CursorType::RESIZE_NESW){
        cursor = [NSCursor respondsToSelector:@selector(_windowResizeNorthEastSouthWestCursor)] ? [NSCursor _windowResizeNorthEastSouthWestCursor] : [NSCursor closedHandCursor];
    }else if (type == quantum::CursorType::RESIZE_ALL){
        cursor = [NSCursor closedHandCursor];
    }else if (type == quantum::CursorType::NOT_ALLOWED){
        cursor = [NSCursor operationNotAllowedCursor];
    }

    [cursor set];
#endif
}

void QuantumApple::setMouseMode(quantum::MouseMode mode){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    // macOS has no built-in cursor confinement, so CONFINED behaves as NORMAL.
    const bool locked = (mode == quantum::MouseMode::CAPTURED);
    if (appleMouseLocked != locked){
        CGAssociateMouseAndMouseCursorPosition(!locked);
        appleMouseLocked = locked;
    }

    const bool hidden = (mode == quantum::MouseMode::HIDDEN || mode == quantum::MouseMode::CAPTURED);
    setAppleCursorHidden(hidden);
#endif
}

int QuantumApple::getScreenWidth(){
    return Renderer.screenSize.width;
}

int QuantumApple::getScreenHeight(){
    return Renderer.screenSize.height;
}

int QuantumApple::getSampleCount(){
    return (int)Renderer.view.sampleCount;
}

bool QuantumApple::isTouchDevice(){
#if TARGET_OS_IPHONE
    return true;
#else
    return false;
#endif
}

void QuantumApple::showVirtualKeyboard(std::wstring text){
#if TARGET_OS_IPHONE
    [Renderer.view becomeFirstResponder];
#endif
}

void QuantumApple::hideVirtualKeyboard(){
#if TARGET_OS_IPHONE
    [Renderer.view resignFirstResponder];
#endif
}

void QuantumApple::quit(){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    // deferred to not close inside a script call
    dispatch_async(dispatch_get_main_queue(), ^{
        [[Renderer.view window] close];
    });
#else
    // iOS apps should not terminate themselves
    quantum::System::quit();
#endif
}

std::string QuantumApple::getAssetPath(){
    NSMutableString* adjusted_relative_path = [[NSMutableString alloc] initWithString:@"assets"];

    NSString* path = [[NSBundle mainBundle] pathForResource:adjusted_relative_path ofType:nil];

    if (!path){
        // No bundle resource: either a plain (non-.app) build, or a project run
        // from outside the editor, which leaves the assets in the project
        // directory and bakes its path in (see Generator::getPlatformCMakeConfig).
#ifdef QUANTUM_ASSET_PATH
        return QUANTUM_ASSET_PATH;
#else
        return "assets";
#endif
    }

    return [path cStringUsingEncoding:NSASCIIStringEncoding];
}

std::string QuantumApple::getUserDataPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirectory = [paths objectAtIndex:0];
    
    return [documentsDirectory UTF8String];
}

std::string QuantumApple::getLuaPath(){
    NSMutableString* adjusted_relative_path = [[NSMutableString alloc] initWithString:@"lua"];

    NSString* path = [[NSBundle mainBundle] pathForResource:adjusted_relative_path ofType:nil];

    if (!path){
#ifdef QUANTUM_LUA_PATH
        return QUANTUM_LUA_PATH;
#else
        return "lua";
#endif
    }

    return [path cStringUsingEncoding:NSASCIIStringEncoding];
}

std::string QuantumApple::getShaderPath(){
    NSMutableString* adjusted_relative_path = [[NSMutableString alloc] initWithString:@"shaders"];

    NSString* path = [[NSBundle mainBundle] pathForResource:adjusted_relative_path ofType:nil];

    if (!path){
#ifdef QUANTUM_SHADER_PATH
        return QUANTUM_SHADER_PATH;
#else
        return System::getShaderPath();
#endif
    }

    return [path cStringUsingEncoding:NSASCIIStringEncoding];
}

bool QuantumApple::getBoolForKey(const char *key, bool defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value boolValue];
    }

    return defaultValue;
}

int QuantumApple::getIntegerForKey(const char *key, int defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value intValue];
    }

    return defaultValue;
}

long QuantumApple::getLongForKey(const char *key, long defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value longValue];
    }

    return defaultValue;
}

float QuantumApple::getFloatForKey(const char *key, float defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value floatValue];
    }

    return defaultValue;
}

double QuantumApple::getDoubleForKey(const char *key, double defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value doubleValue];
    }

    return defaultValue;
}

quantum::Data QuantumApple::getDataForKey(const char* key, const quantum::Data& defaultValue){
    NSData *data = [[NSUserDefaults standardUserDefaults] dataForKey:[NSString stringWithUTF8String:key]];
    if (data){
        return quantum::Data((unsigned char*)data.bytes, (unsigned int)data.length, true, true);
    }
    
     return defaultValue;
}

std::string QuantumApple::getStringForKey(const char *key, const std::string& defaultValue){
    NSString *str = [[NSUserDefaults standardUserDefaults] stringForKey:[NSString stringWithUTF8String:key]];
    if (str){
        return [str UTF8String];
    }
    
    return defaultValue;
}

void QuantumApple::setBoolForKey(const char *key, bool value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithBool:value] forKey:[NSString stringWithUTF8String:key]];
}

void QuantumApple::setIntegerForKey(const char *key, int value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithInt:value] forKey:[NSString stringWithUTF8String:key]];
}

void QuantumApple::setLongForKey(const char *key, long value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithLong:value] forKey:[NSString stringWithUTF8String:key]];
}

void QuantumApple::setFloatForKey(const char *key, float value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithFloat:value] forKey:[NSString stringWithUTF8String:key]];
}

void QuantumApple::setDoubleForKey(const char *key, double value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithDouble:value] forKey:[NSString stringWithUTF8String:key]];
}

void QuantumApple::setDataForKey(const char* key, quantum::Data& value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSData dataWithBytes: value.getMemPtr() length: value.length()] forKey:[NSString stringWithUTF8String:key]];
}

void QuantumApple::setStringForKey(const char* key, const std::string& value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSString stringWithUTF8String:value.c_str()] forKey:[NSString stringWithUTF8String:key]];
}

void QuantumApple::removeKey(const char *key){
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:[NSString stringWithUTF8String:key]];
}

quantum::AdMobBackend* QuantumApple::getAdMobBackend(){
#ifdef QUANTUM_IOS_ADMOB
    return &appleAdMob;
#else
    return nullptr;
#endif
}

quantum::InAppPurchaseBackend* QuantumApple::getInAppPurchaseBackend(){
#ifdef QUANTUM_IOS_STOREKIT
    return &appleInAppPurchase;
#else
    return nullptr;
#endif
}
