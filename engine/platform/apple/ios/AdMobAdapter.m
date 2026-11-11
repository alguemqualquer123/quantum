// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#import "AdMobAdapter.h"

#ifdef QUANTUM_ADMOB

#import <UIKit/UIKit.h>

#import "Renderer.h"

@import GoogleMobileAds;
@import UserMessagingPlatform;

// quantum::AdMobFormat
enum {
    FormatBanner,
    FormatInterstitial,
    FormatRewarded,
    FormatRewardedInterstitial,
    FormatAppOpen,
    FormatCount
};

// quantum::AdMobBannerSize
enum {
    SizeAdaptive,
    SizeBanner,
    SizeLargeBanner,
    SizeMediumRectangle,
    SizeFullBanner,
    SizeLeaderboard
};

// quantum::AdMobBannerPosition
enum {
    PositionTop,
    PositionBottom,
    PositionTopLeft,
    PositionTopRight,
    PositionBottomLeft,
    PositionBottomRight,
    PositionCenter
};

// quantum::AdMob error codes
static const int ErrorNotAvailable = -1;
static const int ErrorNotLoaded = -2;

static void runOnMainThread(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
    } else {
        dispatch_async(dispatch_get_main_queue(), block);
    }
}

static const char *errorMessage(NSError *error) {
    const char *message = error.localizedDescription.UTF8String;
    return message ? message : "";
}

static void notifyConsent(NSError *error) {
    if (error) {
        QuantumAdMobConsentUpdated((int)error.code, errorMessage(error));
    } else {
        QuantumAdMobConsentUpdated(0, "");
    }
}

static GADPaidEventHandler paidEventHandler(int format) {
    return ^(GADAdValue *value) {
        long long micros = [[value.value decimalNumberByMultiplyingByPowerOf10:6] longLongValue];
        const char *currencyCode = value.currencyCode.UTF8String;
        int precision;
        switch (value.precision) {
            case GADAdValuePrecisionEstimated: precision = 1; break;
            case GADAdValuePrecisionPublisherProvided: precision = 2; break;
            case GADAdValuePrecisionPrecise: precision = 3; break;
            default: precision = 0; break;
        }
        QuantumAdMobAdPaid(format, micros, currencyCode ? currencyCode : "", precision);
    };
}

@interface AdMobAdapter () <GADFullScreenContentDelegate, GADBannerViewDelegate>

@property(nonatomic, strong) GADInterstitialAd *interstitialAd;
@property(nonatomic, strong) GADRewardedAd *rewardedAd;
@property(nonatomic, strong) GADRewardedInterstitialAd *rewardedInterstitialAd;
@property(nonatomic, strong) GADAppOpenAd *appOpenAd;
@property(nonatomic, strong) GADBannerView *bannerView;
@property(nonatomic, strong) NSArray<NSLayoutConstraint *> *bannerConstraints;
@property(nonatomic, copy) NSString *verificationUserId;
@property(nonatomic, copy) NSString *verificationCustomData;

@end

@implementation AdMobAdapter {
    // the engine's generation of each format's current load
    int _loadGenerations[FormatCount];
    int _bannerPosition;
    float _appVolume;
    BOOL _appMuted;
    BOOL _initialized;
    // the game is paused while an ad covers it
    BOOL _pausedForAd;
}

- (nonnull instancetype)init {
    self = [super init];
    if (self) {
        _bannerPosition = PositionBottom;
        _appVolume = 1.0f;
        _verificationUserId = @"";
        _verificationCustomData = @"";
    }
    return self;
}

- (UIViewController *)rootViewController {
    // the window of the engine's view
    UIViewController *controller = Renderer.view.window.rootViewController;
    while (controller.presentedViewController && !controller.presentedViewController.isBeingDismissed) {
        controller = controller.presentedViewController;
    }
    return controller;
}

- (void)pauseForAd {
    if (!_pausedForAd) {
        _pausedForAd = YES;
        [Renderer pauseGame];
    }
}

- (void)resumeAfterAd {
    if (_pausedForAd) {
        _pausedForAd = NO;
        [Renderer resumeGame];
    }
}

- (GADRequest *)buildRequest {
    GADRequest *request = [GADRequest request];
    // Google asks wrapping libraries to name themselves
    request.requestAgent = @"quantum";
    return request;
}

- (void)applyAudio {
    if (!_initialized) return;
    GADMobileAds.sharedInstance.applicationVolume = _appVolume;
    GADMobileAds.sharedInstance.applicationMuted = _appMuted;
}

- (void)initializeAdMob {
    runOnMainThread(^{
        [GADMobileAds.sharedInstance startWithCompletionHandler:^(GADInitializationStatus *status) {
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_initialized = YES;
                [self applyAudio];
                QuantumAdMobInitialized();
            });
        }];
    });
}

- (void)setRequestConfigurationWithRating:(int)rating ageRestriction:(int)ageRestriction personalization:(int)personalization testDeviceIds:(NSArray<NSString *> *)testDeviceIds {
    runOnMainThread(^{
        GADRequestConfiguration *configuration = GADMobileAds.sharedInstance.requestConfiguration;

        switch (rating) {
            case 1: configuration.maxAdContentRating = GADMaxAdContentRatingGeneral; break;
            case 2: configuration.maxAdContentRating = GADMaxAdContentRatingParentalGuidance; break;
            case 3: configuration.maxAdContentRating = GADMaxAdContentRatingTeen; break;
            case 4: configuration.maxAdContentRating = GADMaxAdContentRatingMatureAudience; break;
            default: configuration.maxAdContentRating = nil; break;
        }

        switch (ageRestriction) {
            case 1: configuration.ageRestrictedTreatment = GADAgeRestrictedTreatmentChild; break;
            case 2: configuration.ageRestrictedTreatment = GADAgeRestrictedTreatmentTeen; break;
            default: configuration.ageRestrictedTreatment = GADAgeRestrictedTreatmentUnspecified; break;
        }

        switch (personalization) {
            case 1: configuration.publisherPrivacyPersonalizationState = GADPublisherPrivacyPersonalizationStateEnabled; break;
            case 2: configuration.publisherPrivacyPersonalizationState = GADPublisherPrivacyPersonalizationStateDisabled; break;
            default: configuration.publisherPrivacyPersonalizationState = GADPublisherPrivacyPersonalizationStateDefault; break;
        }

        configuration.testDeviceIdentifiers = testDeviceIds;
    });
}

- (void)requestConsentUnderAge:(BOOL)underAgeOfConsent debugGeography:(int)debugGeography testDeviceIds:(NSArray<NSString *> *)testDeviceIds {
    runOnMainThread(^{
        UMPRequestParameters *parameters = [[UMPRequestParameters alloc] init];
        parameters.tagForUnderAgeOfConsent = underAgeOfConsent;

        if (debugGeography != 0 || testDeviceIds.count > 0) {
            UMPDebugSettings *debugSettings = [[UMPDebugSettings alloc] init];
            switch (debugGeography) {
                case 1: debugSettings.geography = UMPDebugGeographyEEA; break;
                case 2: debugSettings.geography = UMPDebugGeographyRegulatedUSState; break;
                case 3: debugSettings.geography = UMPDebugGeographyOther; break;
                default: debugSettings.geography = UMPDebugGeographyDisabled; break;
            }
            debugSettings.testDeviceIdentifiers = testDeviceIds;
            parameters.debugSettings = debugSettings;
        }

        [UMPConsentInformation.sharedInstance requestConsentInfoUpdateWithParameters:parameters completionHandler:^(NSError *error) {
            if (error) {
                notifyConsent(error);
                return;
            }
            [UMPConsentForm loadAndPresentIfRequiredFromViewController:[self rootViewController] completionHandler:^(NSError *formError) {
                notifyConsent(formError);
            }];
        }];
    });
}

// quantum::AdMobConsentStatus
- (int)consentStatus {
    switch (UMPConsentInformation.sharedInstance.consentStatus) {
        case UMPConsentStatusRequired: return 1;
        case UMPConsentStatusNotRequired: return 2;
        case UMPConsentStatusObtained: return 3;
        default: return 0;
    }
}

- (BOOL)canRequestAds {
    return UMPConsentInformation.sharedInstance.canRequestAds;
}

- (BOOL)isPrivacyOptionsRequired {
    return UMPConsentInformation.sharedInstance.privacyOptionsRequirementStatus == UMPPrivacyOptionsRequirementStatusRequired;
}

- (void)showPrivacyOptionsForm {
    runOnMainThread(^{
        [UMPConsentForm presentPrivacyOptionsFormFromViewController:[self rootViewController] completionHandler:^(NSError *error) {
            notifyConsent(error);
        }];
    });
}

- (void)resetConsent {
    runOnMainThread(^{
        [UMPConsentInformation.sharedInstance reset];
    });
}

- (id)fullScreenAdForFormat:(int)format {
    switch (format) {
        case FormatInterstitial: return self.interstitialAd;
        case FormatRewarded: return self.rewardedAd;
        case FormatRewardedInterstitial: return self.rewardedInterstitialAd;
        case FormatAppOpen: return self.appOpenAd;
        default: return nil;
    }
}

- (void)setFullScreenAd:(id)ad format:(int)format {
    switch (format) {
        case FormatInterstitial: self.interstitialAd = ad; break;
        case FormatRewarded: self.rewardedAd = ad; break;
        case FormatRewardedInterstitial: self.rewardedInterstitialAd = ad; break;
        case FormatAppOpen: self.appOpenAd = ad; break;
        default: break;
    }
}

- (int)formatOfAd:(id)ad {
    if ([ad isKindOfClass:GADInterstitialAd.class]) return FormatInterstitial;
    if ([ad isKindOfClass:GADRewardedAd.class]) return FormatRewarded;
    if ([ad isKindOfClass:GADRewardedInterstitialAd.class]) return FormatRewardedInterstitial;
    if ([ad isKindOfClass:GADAppOpenAd.class]) return FormatAppOpen;
    return FormatInterstitial;
}

- (void)loadAd:(int)format adUnitId:(NSString *)adUnitId generation:(int)generation {
    runOnMainThread(^{
        // replaces the ad; late answers to older loads are dropped
        self->_loadGenerations[format] = generation;
        [self setFullScreenAd:nil format:format];

        __weak AdMobAdapter *weakSelf = self;
        void (^completion)(id, NSError *) = ^(id ad, NSError *error) {
            AdMobAdapter *strongSelf = weakSelf;
            if (!strongSelf || generation != strongSelf->_loadGenerations[format]) return;

            if (error || !ad) {
                QuantumAdMobAdFailedToLoad(format, generation, (int)error.code, errorMessage(error));
                return;
            }

            id<GADFullScreenPresentingAd> presentingAd = ad;
            presentingAd.fullScreenContentDelegate = strongSelf;
            [ad setPaidEventHandler:paidEventHandler(format)];
            [strongSelf setFullScreenAd:ad format:format];
            QuantumAdMobAdLoaded(format, generation, 0, 0);
        };

        GADRequest *request = [self buildRequest];
        // braced, as ARC forbids jumping past a block's capture
        switch (format) {
            case FormatInterstitial: {
                [GADInterstitialAd loadWithAdUnitID:adUnitId request:request completionHandler:^(GADInterstitialAd *ad, NSError *error) {
                    completion(ad, error);
                }];
                break;
            }
            case FormatRewarded: {
                [GADRewardedAd loadWithAdUnitID:adUnitId request:request completionHandler:^(GADRewardedAd *ad, NSError *error) {
                    completion(ad, error);
                }];
                break;
            }
            case FormatRewardedInterstitial: {
                [GADRewardedInterstitialAd loadWithAdUnitID:adUnitId request:request completionHandler:^(GADRewardedInterstitialAd *ad, NSError *error) {
                    completion(ad, error);
                }];
                break;
            }
            default: {
                [GADAppOpenAd loadWithAdUnitID:adUnitId request:request completionHandler:^(GADAppOpenAd *ad, NSError *error) {
                    completion(ad, error);
                }];
                break;
            }
        }
    });
}

- (GADServerSideVerificationOptions *)verificationOptions {
    if (self.verificationUserId.length == 0 && self.verificationCustomData.length == 0) return nil;

    GADServerSideVerificationOptions *options = [[GADServerSideVerificationOptions alloc] init];
    options.userIdentifier = self.verificationUserId;
    options.customRewardString = self.verificationCustomData;
    return options;
}

- (void)showAd:(int)format {
    runOnMainThread(^{
        id ad = [self fullScreenAdForFormat:format];
        if (!ad) {
            QuantumAdMobAdFailedToShow(format, ErrorNotLoaded, "The ad is not loaded");
            return;
        }
        // shown once
        [self setFullScreenAd:nil format:format];

        UIViewController *controller = [self rootViewController];
        if ([ad isKindOfClass:GADRewardedAd.class]) {
            GADRewardedAd *rewarded = ad;
            __weak GADRewardedAd *weakRewarded = rewarded;
            rewarded.serverSideVerificationOptions = [self verificationOptions];
            [rewarded presentFromRootViewController:controller userDidEarnRewardHandler:^{
                GADAdReward *reward = weakRewarded.adReward;
                const char *type = reward.type.UTF8String;
                QuantumAdMobUserEarnedReward(format, type ? type : "", reward.amount.intValue);
            }];
        } else if ([ad isKindOfClass:GADRewardedInterstitialAd.class]) {
            GADRewardedInterstitialAd *rewarded = ad;
            __weak GADRewardedInterstitialAd *weakRewarded = rewarded;
            rewarded.serverSideVerificationOptions = [self verificationOptions];
            [rewarded presentFromRootViewController:controller userDidEarnRewardHandler:^{
                GADAdReward *reward = weakRewarded.adReward;
                const char *type = reward.type.UTF8String;
                QuantumAdMobUserEarnedReward(format, type ? type : "", reward.amount.intValue);
            }];
        } else if ([ad isKindOfClass:GADInterstitialAd.class]) {
            [(GADInterstitialAd *)ad presentFromRootViewController:controller];
        } else if ([ad isKindOfClass:GADAppOpenAd.class]) {
            [(GADAppOpenAd *)ad presentFromRootViewController:controller];
        }
    });
}

- (GADAdSize)bannerAdSize:(int)size inView:(UIView *)view {
    switch (size) {
        case SizeBanner: return GADAdSizeBanner;
        case SizeLargeBanner: return GADAdSizeLargeBanner;
        case SizeMediumRectangle: return GADAdSizeMediumRectangle;
        case SizeFullBanner: return GADAdSizeFullBanner;
        case SizeLeaderboard: return GADAdSizeLeaderboard;
        default: {
            // centered, so it clears the deeper side inset on both sides
            UIEdgeInsets insets = view.safeAreaInsets;
            CGFloat width = view.bounds.size.width - 2 * MAX(insets.left, insets.right);
            return GADLargeAnchoredAdaptiveBannerAdSizeWithWidth(width);
        }
    }
}

- (void)layoutBanner {
    GADBannerView *banner = self.bannerView;
    UIView *parent = banner.superview;
    if (!banner || !parent) return;

    if (self.bannerConstraints) {
        [NSLayoutConstraint deactivateConstraints:self.bannerConstraints];
    }

    // the safe area keeps the banner clear of the notch and the home indicator
    UILayoutGuide *safeArea = parent.safeAreaLayoutGuide;
    NSLayoutConstraint *horizontal;
    NSLayoutConstraint *vertical;
    switch (_bannerPosition) {
        case PositionTop:
            horizontal = [banner.centerXAnchor constraintEqualToAnchor:safeArea.centerXAnchor];
            vertical = [banner.topAnchor constraintEqualToAnchor:safeArea.topAnchor];
            break;
        case PositionTopLeft:
            horizontal = [banner.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor];
            vertical = [banner.topAnchor constraintEqualToAnchor:safeArea.topAnchor];
            break;
        case PositionTopRight:
            horizontal = [banner.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor];
            vertical = [banner.topAnchor constraintEqualToAnchor:safeArea.topAnchor];
            break;
        case PositionBottomLeft:
            horizontal = [banner.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor];
            vertical = [banner.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor];
            break;
        case PositionBottomRight:
            horizontal = [banner.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor];
            vertical = [banner.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor];
            break;
        case PositionCenter:
            horizontal = [banner.centerXAnchor constraintEqualToAnchor:safeArea.centerXAnchor];
            vertical = [banner.centerYAnchor constraintEqualToAnchor:safeArea.centerYAnchor];
            break;
        default:
            horizontal = [banner.centerXAnchor constraintEqualToAnchor:safeArea.centerXAnchor];
            vertical = [banner.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor];
            break;
    }

    self.bannerConstraints = @[horizontal, vertical];
    [NSLayoutConstraint activateConstraints:self.bannerConstraints];
}

- (void)destroyBanner {
    if (!self.bannerView) return;
    if (self.bannerConstraints) {
        [NSLayoutConstraint deactivateConstraints:self.bannerConstraints];
        self.bannerConstraints = nil;
    }
    self.bannerView.delegate = nil;
    [self.bannerView removeFromSuperview];
    self.bannerView = nil;
}

- (void)loadBanner:(NSString *)adUnitId size:(int)size position:(int)position visible:(BOOL)visible generation:(int)generation {
    runOnMainThread(^{
        [self destroyBanner];
        self->_bannerPosition = position;
        self->_loadGenerations[FormatBanner] = generation;

        UIViewController *controller = [self rootViewController];
        UIView *parent = controller.view;
        if (!parent) {
            QuantumAdMobAdFailedToLoad(FormatBanner, generation, ErrorNotAvailable, "No view to show the banner in");
            return;
        }

        GADBannerView *banner = [[GADBannerView alloc] initWithAdSize:[self bannerAdSize:size inView:parent]];
        banner.adUnitID = adUnitId;
        banner.rootViewController = controller;
        banner.delegate = self;
        banner.paidEventHandler = paidEventHandler(FormatBanner);
        banner.hidden = !visible;
        banner.translatesAutoresizingMaskIntoConstraints = NO;

        [parent addSubview:banner];
        self.bannerView = banner;
        [self layoutBanner];

        [banner loadRequest:[self buildRequest]];
    });
}

- (void)setBannerVisible:(BOOL)visible {
    runOnMainThread(^{
        self.bannerView.hidden = !visible;
    });
}

- (void)setBannerPosition:(int)position {
    runOnMainThread(^{
        self->_bannerPosition = position;
        [self layoutBanner];
    });
}

- (void)removeBanner {
    runOnMainThread(^{
        [self destroyBanner];
    });
}

- (void)setServerSideVerificationUserId:(NSString *)userId customData:(NSString *)customData {
    runOnMainThread(^{
        self.verificationUserId = userId;
        self.verificationCustomData = customData;
    });
}

- (void)setAppVolume:(float)volume {
    runOnMainThread(^{
        self->_appVolume = MAX(0.0f, MIN(1.0f, volume));
        [self applyAudio];
    });
}

- (void)setAppMuted:(BOOL)muted {
    runOnMainThread(^{
        self->_appMuted = muted;
        [self applyAudio];
    });
}

- (void)openAdInspector {
    runOnMainThread(^{
        [GADMobileAds.sharedInstance presentAdInspectorFromViewController:[self rootViewController] completionHandler:^(NSError *error) {
            if (error) {
                QuantumAdMobAdInspectorClosed((int)error.code, errorMessage(error));
            } else {
                QuantumAdMobAdInspectorClosed(0, "");
            }
        }];
    });
}

#pragma mark - GADFullScreenContentDelegate

- (void)adWillPresentFullScreenContent:(id<GADFullScreenPresentingAd>)ad {
    [self pauseForAd];
    QuantumAdMobAdShown([self formatOfAd:ad]);
}

- (void)ad:(id<GADFullScreenPresentingAd>)ad didFailToPresentFullScreenContentWithError:(NSError *)error {
    [self resumeAfterAd];
    QuantumAdMobAdFailedToShow([self formatOfAd:ad], (int)error.code, errorMessage(error));
}

- (void)adDidDismissFullScreenContent:(id<GADFullScreenPresentingAd>)ad {
    [self resumeAfterAd];
    QuantumAdMobAdDismissed([self formatOfAd:ad]);
}

- (void)adDidRecordImpression:(id<GADFullScreenPresentingAd>)ad {
    QuantumAdMobAdImpression([self formatOfAd:ad]);
}

- (void)adDidRecordClick:(id<GADFullScreenPresentingAd>)ad {
    QuantumAdMobAdClicked([self formatOfAd:ad]);
}

#pragma mark - GADBannerViewDelegate

- (void)bannerViewDidReceiveAd:(GADBannerView *)bannerView {
    if (bannerView != self.bannerView) return;
    // pixels, like the engine's screen size
    CGFloat scale = Renderer.view ? Renderer.view.contentScaleFactor : UIScreen.mainScreen.scale;
    CGSize size = CGSizeFromGADAdSize(bannerView.adSize);
    QuantumAdMobAdLoaded(FormatBanner, _loadGenerations[FormatBanner], (int)lround(size.width * scale), (int)lround(size.height * scale));
}

- (void)bannerView:(GADBannerView *)bannerView didFailToReceiveAdWithError:(NSError *)error {
    if (bannerView != self.bannerView) return;
    QuantumAdMobAdFailedToLoad(FormatBanner, _loadGenerations[FormatBanner], (int)error.code, errorMessage(error));
}

- (void)bannerViewDidRecordImpression:(GADBannerView *)bannerView {
    QuantumAdMobAdImpression(FormatBanner);
}

- (void)bannerViewDidRecordClick:(GADBannerView *)bannerView {
    QuantumAdMobAdClicked(FormatBanner);
}

- (void)bannerViewWillPresentScreen:(GADBannerView *)bannerView {
    [self pauseForAd];
    QuantumAdMobAdShown(FormatBanner);
}

- (void)bannerViewDidDismissScreen:(GADBannerView *)bannerView {
    [self resumeAfterAd];
    QuantumAdMobAdDismissed(FormatBanner);
}

@end

#endif // QUANTUM_ADMOB
