package org.srvinix.quantum;

import android.app.Activity;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.annotation.NonNull;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.gms.ads.AdError;
import com.google.android.gms.ads.AdListener;
import com.google.android.gms.ads.AdRequest;
import com.google.android.gms.ads.AdSize;
import com.google.android.gms.ads.AdValue;
import com.google.android.gms.ads.AdView;
import com.google.android.gms.ads.AgeRestrictedTreatment;
import com.google.android.gms.ads.FullScreenContentCallback;
import com.google.android.gms.ads.LoadAdError;
import com.google.android.gms.ads.MobileAds;
import com.google.android.gms.ads.RequestConfiguration;
import com.google.android.gms.ads.appopen.AppOpenAd;
import com.google.android.gms.ads.interstitial.InterstitialAd;
import com.google.android.gms.ads.interstitial.InterstitialAdLoadCallback;
import com.google.android.gms.ads.rewarded.RewardedAd;
import com.google.android.gms.ads.rewarded.RewardedAdLoadCallback;
import com.google.android.gms.ads.rewarded.ServerSideVerificationOptions;
import com.google.android.gms.ads.rewardedinterstitial.RewardedInterstitialAd;
import com.google.android.gms.ads.rewardedinterstitial.RewardedInterstitialAdLoadCallback;
import com.google.android.ump.ConsentDebugSettings;
import com.google.android.ump.ConsentInformation;
import com.google.android.ump.ConsentRequestParameters;
import com.google.android.ump.FormError;
import com.google.android.ump.UserMessagingPlatform;

import java.util.Arrays;

// Google Mobile Ads and UMP for quantum::AdMob, called from the engine thread
public class AdMobWrapper implements ActivityListener {

	// quantum::AdMobFormat
	private static final int FORMAT_BANNER = 0;
	private static final int FORMAT_INTERSTITIAL = 1;
	private static final int FORMAT_REWARDED = 2;
	private static final int FORMAT_REWARDED_INTERSTITIAL = 3;
	private static final int FORMAT_APP_OPEN = 4;
	private static final int FORMAT_COUNT = 5;

	// quantum::AdMobBannerSize
	private static final int SIZE_BANNER = 1;
	private static final int SIZE_LARGE_BANNER = 2;
	private static final int SIZE_MEDIUM_RECTANGLE = 3;
	private static final int SIZE_FULL_BANNER = 4;
	private static final int SIZE_LEADERBOARD = 5;

	// quantum::AdMobBannerPosition
	private static final int POSITION_TOP = 0;
	private static final int POSITION_BOTTOM = 1;
	private static final int POSITION_TOP_LEFT = 2;
	private static final int POSITION_TOP_RIGHT = 3;
	private static final int POSITION_BOTTOM_LEFT = 4;
	private static final int POSITION_BOTTOM_RIGHT = 5;

	// quantum::AdMob error codes
	private static final int ERROR_NOT_AVAILABLE = -1;
	private static final int ERROR_NOT_LOADED = -2;

	private final Activity activity;
	private final ConsentInformation consentInformation;

	// UI thread only
	private final Object[] fullScreenAds = new Object[FORMAT_COUNT];
	// the engine's generation of each format's current load
	private final int[] loadGenerations = new int[FORMAT_COUNT];
	private AdView bannerView;
	private int bannerPosition = POSITION_BOTTOM;
	private String verificationUserId = "";
	private String verificationCustomData = "";
	private float appVolume = 1.0f;
	private boolean appMuted = false;
	private boolean initialized = false;

	public AdMobWrapper(Activity activity) {
		this.activity = activity;
		consentInformation = UserMessagingPlatform.getConsentInformation(activity);
	}

	public void initialize() {
		activity.runOnUiThread(() -> {
			// Google asks for a background thread
			new Thread(() -> MobileAds.initialize(activity, status -> activity.runOnUiThread(() -> {
				initialized = true;
				applyAudio();
				nativeOnInitialized();
			}))).start();
		});
	}

	public void setRequestConfiguration(int rating, int ageRestriction, int personalization, String[] testDeviceIds) {
		activity.runOnUiThread(() -> {
			RequestConfiguration configuration = MobileAds.getRequestConfiguration().toBuilder()
					.setMaxAdContentRating(maxAdContentRating(rating))
					.setAgeRestrictedTreatment(ageRestrictedTreatment(ageRestriction))
					.setPublisherPrivacyPersonalizationState(personalizationState(personalization))
					.setTestDeviceIds(Arrays.asList(testDeviceIds))
					.build();
			MobileAds.setRequestConfiguration(configuration);
		});
	}

	public void requestConsent(boolean underAgeOfConsent, int debugGeography, String[] testDeviceIds) {
		activity.runOnUiThread(() -> {
			ConsentRequestParameters.Builder parameters = new ConsentRequestParameters.Builder()
					.setTagForUnderAgeOfConsent(underAgeOfConsent);

			if (debugGeography != 0 || testDeviceIds.length > 0) {
				ConsentDebugSettings.Builder debugSettings = new ConsentDebugSettings.Builder(activity)
						.setDebugGeography(debugGeography(debugGeography));
				for (String id : testDeviceIds) {
					debugSettings.addTestDeviceHashedId(id);
				}
				parameters.setConsentDebugSettings(debugSettings.build());
			}

			consentInformation.requestConsentInfoUpdate(activity, parameters.build(),
					() -> UserMessagingPlatform.loadAndShowConsentFormIfRequired(activity, AdMobWrapper::notifyConsent),
					AdMobWrapper::notifyConsent);
		});
	}

	// quantum::AdMobConsentStatus
	public int getConsentStatus() {
		switch (consentInformation.getConsentStatus()) {
			case ConsentInformation.ConsentStatus.REQUIRED: return 1;
			case ConsentInformation.ConsentStatus.NOT_REQUIRED: return 2;
			case ConsentInformation.ConsentStatus.OBTAINED: return 3;
			default: return 0;
		}
	}

	public boolean canRequestAds() {
		return consentInformation.canRequestAds();
	}

	public boolean isPrivacyOptionsRequired() {
		return consentInformation.getPrivacyOptionsRequirementStatus()
				== ConsentInformation.PrivacyOptionsRequirementStatus.REQUIRED;
	}

	public void showPrivacyOptionsForm() {
		activity.runOnUiThread(() -> UserMessagingPlatform.showPrivacyOptionsForm(activity, AdMobWrapper::notifyConsent));
	}

	public void resetConsent() {
		activity.runOnUiThread(consentInformation::reset);
	}

	public void loadAd(int format, String adUnitId, int generation) {
		activity.runOnUiThread(() -> {
			// replaces the ad; late answers to older loads are dropped
			loadGenerations[format] = generation;
			fullScreenAds[format] = null;
			AdRequest request = buildRequest();

			if (format == FORMAT_INTERSTITIAL) {
				InterstitialAd.load(activity, adUnitId, request, new InterstitialAdLoadCallback() {
					@Override
					public void onAdLoaded(@NonNull InterstitialAd ad) {
						ad.setFullScreenContentCallback(new FullScreenCallback(format));
						ad.setOnPaidEventListener(value -> notifyPaid(format, value));
						ad.setImmersiveMode(true);
						adLoaded(format, generation, ad);
					}

					@Override
					public void onAdFailedToLoad(@NonNull LoadAdError error) {
						adFailedToLoad(format, generation, error);
					}
				});
			} else if (format == FORMAT_REWARDED) {
				RewardedAd.load(activity, adUnitId, request, new RewardedAdLoadCallback() {
					@Override
					public void onAdLoaded(@NonNull RewardedAd ad) {
						ad.setFullScreenContentCallback(new FullScreenCallback(format));
						ad.setOnPaidEventListener(value -> notifyPaid(format, value));
						ad.setImmersiveMode(true);
						adLoaded(format, generation, ad);
					}

					@Override
					public void onAdFailedToLoad(@NonNull LoadAdError error) {
						adFailedToLoad(format, generation, error);
					}
				});
			} else if (format == FORMAT_REWARDED_INTERSTITIAL) {
				RewardedInterstitialAd.load(activity, adUnitId, request, new RewardedInterstitialAdLoadCallback() {
					@Override
					public void onAdLoaded(@NonNull RewardedInterstitialAd ad) {
						ad.setFullScreenContentCallback(new FullScreenCallback(format));
						ad.setOnPaidEventListener(value -> notifyPaid(format, value));
						ad.setImmersiveMode(true);
						adLoaded(format, generation, ad);
					}

					@Override
					public void onAdFailedToLoad(@NonNull LoadAdError error) {
						adFailedToLoad(format, generation, error);
					}
				});
			} else {
				AppOpenAd.load(activity, adUnitId, request, new AppOpenAd.AppOpenAdLoadCallback() {
					@Override
					public void onAdLoaded(@NonNull AppOpenAd ad) {
						ad.setFullScreenContentCallback(new FullScreenCallback(format));
						ad.setOnPaidEventListener(value -> notifyPaid(format, value));
						ad.setImmersiveMode(true);
						adLoaded(format, generation, ad);
					}

					@Override
					public void onAdFailedToLoad(@NonNull LoadAdError error) {
						adFailedToLoad(format, generation, error);
					}
				});
			}
		});
	}

	public void showAd(int format) {
		activity.runOnUiThread(() -> {
			Object ad = fullScreenAds[format];
			if (ad == null) {
				nativeOnAdFailedToShow(format, ERROR_NOT_LOADED, "The ad is not loaded");
				return;
			}
			// shown once
			fullScreenAds[format] = null;

			// ads seen by Firebase Test Lab robots count as invalid traffic
			if ("true".equals(Settings.System.getString(activity.getContentResolver(), "firebase.test.lab"))) {
				nativeOnAdFailedToShow(format, ERROR_NOT_AVAILABLE, "Ads are not shown in Firebase Test Lab");
				return;
			}

			if (ad instanceof InterstitialAd) {
				((InterstitialAd) ad).show(activity);
			} else if (ad instanceof RewardedAd) {
				RewardedAd rewarded = (RewardedAd) ad;
				ServerSideVerificationOptions options = verificationOptions();
				if (options != null) rewarded.setServerSideVerificationOptions(options);
				rewarded.show(activity, reward -> nativeOnUserEarnedReward(format, reward.getType(), reward.getAmount()));
			} else if (ad instanceof RewardedInterstitialAd) {
				RewardedInterstitialAd rewarded = (RewardedInterstitialAd) ad;
				ServerSideVerificationOptions options = verificationOptions();
				if (options != null) rewarded.setServerSideVerificationOptions(options);
				rewarded.show(activity, reward -> nativeOnUserEarnedReward(format, reward.getType(), reward.getAmount()));
			} else if (ad instanceof AppOpenAd) {
				((AppOpenAd) ad).show(activity);
			}
		});
	}

	public void loadBanner(String adUnitId, int size, int position, boolean visible, int generation) {
		activity.runOnUiThread(() -> {
			destroyBanner();
			bannerPosition = position;

			final AdView view = new AdView(activity);
			view.setAdUnitId(adUnitId);
			view.setAdSize(bannerAdSize(size));
			view.setAdListener(new AdListener() {
				@Override
				public void onAdLoaded() {
					if (bannerView != view) return;
					AdSize adSize = view.getAdSize();
					int width = (adSize != null) ? adSize.getWidthInPixels(activity) : view.getWidth();
					int height = (adSize != null) ? adSize.getHeightInPixels(activity) : view.getHeight();
					nativeOnAdLoaded(FORMAT_BANNER, generation, width, height);
				}

				@Override
				public void onAdFailedToLoad(@NonNull LoadAdError error) {
					if (bannerView != view) return;
					nativeOnAdFailedToLoad(FORMAT_BANNER, generation, error.getCode(), error.getMessage());
				}

				@Override
				public void onAdOpened() {
					nativeOnAdShown(FORMAT_BANNER);
				}

				@Override
				public void onAdClosed() {
					nativeOnAdDismissed(FORMAT_BANNER);
				}

				@Override
				public void onAdClicked() {
					nativeOnAdClicked(FORMAT_BANNER);
				}

				@Override
				public void onAdImpression() {
					nativeOnAdImpression(FORMAT_BANNER);
				}
			});
			view.setOnPaidEventListener(value -> notifyPaid(FORMAT_BANNER, value));
			view.setVisibility(visible ? View.VISIBLE : View.GONE);

			// keeps the banner out of display cutouts and visible system bars
			ViewCompat.setOnApplyWindowInsetsListener(view, (v, insets) -> {
				if (bannerView == view) {
					view.setLayoutParams(bannerLayoutParams(safeInsets(insets)));
				}
				return insets;
			});

			bannerView = view;
			activity.addContentView(view, bannerLayoutParams(safeInsets(null)));
			view.loadAd(buildRequest());
		});
	}

	public void setBannerVisible(boolean visible) {
		activity.runOnUiThread(() -> {
			if (bannerView != null) {
				bannerView.setVisibility(visible ? View.VISIBLE : View.GONE);
			}
		});
	}

	public void setBannerPosition(int position) {
		activity.runOnUiThread(() -> {
			bannerPosition = position;
			if (bannerView != null) {
				bannerView.setLayoutParams(bannerLayoutParams(safeInsets(null)));
			}
		});
	}

	public void removeBanner() {
		activity.runOnUiThread(this::destroyBanner);
	}

	public void setServerSideVerificationOptions(String userId, String customData) {
		activity.runOnUiThread(() -> {
			verificationUserId = userId;
			verificationCustomData = customData;
		});
	}

	public void setAppVolume(float volume) {
		activity.runOnUiThread(() -> {
			appVolume = Math.max(0.0f, Math.min(1.0f, volume));
			applyAudio();
		});
	}

	public void setAppMuted(boolean muted) {
		activity.runOnUiThread(() -> {
			appMuted = muted;
			applyAudio();
		});
	}

	public void openAdInspector() {
		activity.runOnUiThread(() -> MobileAds.openAdInspector(activity, error -> {
			if (error == null) {
				nativeOnAdInspectorClosed(0, "");
			} else {
				nativeOnAdInspectorClosed(error.getCode(), error.getMessage());
			}
		}));
	}

	@Override
	public void onActivityResume() {
		if (bannerView != null) bannerView.resume();
	}

	@Override
	public void onActivityPause() {
		if (bannerView != null) bannerView.pause();
	}

	@Override
	public void onActivityDestroy() {
		destroyBanner();
	}

	private AdRequest buildRequest() {
		// Google asks wrapping libraries to name themselves
		return new AdRequest.Builder().setRequestAgent("quantum").build();
	}

	private void adLoaded(int format, int generation, Object ad) {
		if (generation != loadGenerations[format]) return;
		fullScreenAds[format] = ad;
		nativeOnAdLoaded(format, generation, 0, 0);
	}

	private void adFailedToLoad(int format, int generation, LoadAdError error) {
		if (generation != loadGenerations[format]) return;
		nativeOnAdFailedToLoad(format, generation, error.getCode(), error.getMessage());
	}

	private ServerSideVerificationOptions verificationOptions() {
		if (verificationUserId.isEmpty() && verificationCustomData.isEmpty()) return null;
		return new ServerSideVerificationOptions.Builder()
				.setUserId(verificationUserId)
				.setCustomData(verificationCustomData)
				.build();
	}

	// The SDK throws when audio is set before it finishes initializing
	private void applyAudio() {
		if (!initialized) return;
		MobileAds.setAppVolume(appVolume);
		MobileAds.setAppMuted(appMuted);
	}

	private void destroyBanner() {
		if (bannerView == null) return;
		ViewGroup parent = (ViewGroup) bannerView.getParent();
		if (parent != null) parent.removeView(bannerView);
		bannerView.destroy();
		bannerView = null;
	}

	private Insets safeInsets(WindowInsetsCompat insets) {
		if (insets == null) {
			insets = ViewCompat.getRootWindowInsets(activity.getWindow().getDecorView());
		}
		if (insets == null) return Insets.NONE;
		// hidden system bars report zero
		return insets.getInsets(WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
	}

	private AdSize bannerAdSize(int size) {
		switch (size) {
			case SIZE_BANNER: return AdSize.BANNER;
			case SIZE_LARGE_BANNER: return AdSize.LARGE_BANNER;
			case SIZE_MEDIUM_RECTANGLE: return AdSize.MEDIUM_RECTANGLE;
			case SIZE_FULL_BANNER: return AdSize.FULL_BANNER;
			case SIZE_LEADERBOARD: return AdSize.LEADERBOARD;
			default: {
				DisplayMetrics metrics = activity.getResources().getDisplayMetrics();
				int screenWidth = activity.getWindow().getDecorView().getWidth();
				if (screenWidth <= 0) screenWidth = metrics.widthPixels;
				// centered, so it clears the deeper side inset on both sides
				Insets insets = safeInsets(null);
				int width = screenWidth - 2 * Math.max(insets.left, insets.right);
				return AdSize.getLargeAnchoredAdaptiveBannerAdSize(activity, (int) (width / metrics.density));
			}
		}
	}

	private FrameLayout.LayoutParams bannerLayoutParams(Insets insets) {
		int horizontal;
		int vertical;
		switch (bannerPosition) {
			case POSITION_TOP: horizontal = Gravity.CENTER_HORIZONTAL; vertical = Gravity.TOP; break;
			case POSITION_BOTTOM: horizontal = Gravity.CENTER_HORIZONTAL; vertical = Gravity.BOTTOM; break;
			case POSITION_TOP_LEFT: horizontal = Gravity.LEFT; vertical = Gravity.TOP; break;
			case POSITION_TOP_RIGHT: horizontal = Gravity.RIGHT; vertical = Gravity.TOP; break;
			case POSITION_BOTTOM_LEFT: horizontal = Gravity.LEFT; vertical = Gravity.BOTTOM; break;
			case POSITION_BOTTOM_RIGHT: horizontal = Gravity.RIGHT; vertical = Gravity.BOTTOM; break;
			default: horizontal = Gravity.CENTER_HORIZONTAL; vertical = Gravity.CENTER_VERTICAL; break;
		}

		FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, horizontal | vertical);
		params.leftMargin = (horizontal == Gravity.LEFT) ? insets.left : 0;
		params.rightMargin = (horizontal == Gravity.RIGHT) ? insets.right : 0;
		params.topMargin = (vertical == Gravity.TOP) ? insets.top : 0;
		params.bottomMargin = (vertical == Gravity.BOTTOM) ? insets.bottom : 0;
		return params;
	}

	private static String maxAdContentRating(int rating) {
		switch (rating) {
			case 1: return RequestConfiguration.MAX_AD_CONTENT_RATING_G;
			case 2: return RequestConfiguration.MAX_AD_CONTENT_RATING_PG;
			case 3: return RequestConfiguration.MAX_AD_CONTENT_RATING_T;
			case 4: return RequestConfiguration.MAX_AD_CONTENT_RATING_MA;
			default: return RequestConfiguration.MAX_AD_CONTENT_RATING_UNSPECIFIED;
		}
	}

	private static AgeRestrictedTreatment ageRestrictedTreatment(int ageRestriction) {
		switch (ageRestriction) {
			case 1: return AgeRestrictedTreatment.CHILD;
			case 2: return AgeRestrictedTreatment.TEEN;
			default: return AgeRestrictedTreatment.UNSPECIFIED;
		}
	}

	private static RequestConfiguration.PublisherPrivacyPersonalizationState personalizationState(int personalization) {
		switch (personalization) {
			case 1: return RequestConfiguration.PublisherPrivacyPersonalizationState.ENABLED;
			case 2: return RequestConfiguration.PublisherPrivacyPersonalizationState.DISABLED;
			default: return RequestConfiguration.PublisherPrivacyPersonalizationState.DEFAULT;
		}
	}

	private static int debugGeography(int geography) {
		switch (geography) {
			case 1: return ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_EEA;
			case 2: return ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_REGULATED_US_STATE;
			case 3: return ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_OTHER;
			default: return ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_DISABLED;
		}
	}

	private static void notifyConsent(FormError error) {
		if (error == null) {
			nativeOnConsentUpdated(0, "");
		} else {
			nativeOnConsentUpdated(error.getErrorCode(), error.getMessage());
		}
	}

	private static void notifyPaid(int format, AdValue value) {
		nativeOnAdPaid(format, value.getValueMicros(), value.getCurrencyCode(), value.getPrecisionType());
	}

	private static class FullScreenCallback extends FullScreenContentCallback {
		private final int format;

		FullScreenCallback(int format) {
			this.format = format;
		}

		@Override
		public void onAdShowedFullScreenContent() {
			nativeOnAdShown(format);
		}

		@Override
		public void onAdFailedToShowFullScreenContent(@NonNull AdError error) {
			nativeOnAdFailedToShow(format, error.getCode(), error.getMessage());
		}

		@Override
		public void onAdDismissedFullScreenContent() {
			nativeOnAdDismissed(format);
		}

		@Override
		public void onAdClicked() {
			nativeOnAdClicked(format);
		}

		@Override
		public void onAdImpression() {
			nativeOnAdImpression(format);
		}
	}

	// Registered by AndroidServices.cpp
	private static native void nativeOnInitialized();
	private static native void nativeOnConsentUpdated(int errorCode, String message);
	private static native void nativeOnAdLoaded(int format, int generation, int width, int height);
	private static native void nativeOnAdFailedToLoad(int format, int generation, int errorCode, String message);
	private static native void nativeOnAdShown(int format);
	private static native void nativeOnAdFailedToShow(int format, int errorCode, String message);
	private static native void nativeOnAdDismissed(int format);
	private static native void nativeOnAdClicked(int format);
	private static native void nativeOnAdImpression(int format);
	private static native void nativeOnAdPaid(int format, long valueMicros, String currencyCode, int precision);
	private static native void nativeOnUserEarnedReward(int format, String type, int amount);
	private static native void nativeOnAdInspectorClosed(int errorCode, String message);
}
