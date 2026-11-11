package org.srvinix.quantum;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import androidx.annotation.NonNull;

import com.android.billingclient.api.AccountIdentifiers;
import com.android.billingclient.api.AcknowledgePurchaseParams;
import com.android.billingclient.api.BillingClient;
import com.android.billingclient.api.BillingClientStateListener;
import com.android.billingclient.api.BillingFlowParams;
import com.android.billingclient.api.BillingResult;
import com.android.billingclient.api.ConsumeParams;
import com.android.billingclient.api.InAppMessageParams;
import com.android.billingclient.api.InAppMessageResult;
import com.android.billingclient.api.PendingPurchasesParams;
import com.android.billingclient.api.ProductDetails;
import com.android.billingclient.api.Purchase;
import com.android.billingclient.api.PurchasesUpdatedListener;
import com.android.billingclient.api.QueryProductDetailsParams;
import com.android.billingclient.api.QueryPurchasesParams;
import com.android.billingclient.api.UnfetchedProduct;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

// Google Play Billing for quantum::InAppPurchase, called from the engine thread
public class BillingWrapper implements PurchasesUpdatedListener, ActivityListener {
	private static final String TAG = "Quantum";
	private static final String NOT_INITIALIZED = "Call InAppPurchase.initialize first";

	// quantum::ProductType
	private static final int TYPE_INAPP = 0;
	private static final int TYPE_SUBS = 1;
	// waiting for a lookup, never sent
	private static final int TYPE_UNKNOWN = -1;

	private static final long LOOKUP_RETRY_MS = 5000;

	// quantum::RecurrenceMode
	private static final int RECURRENCE_INFINITE = 0;
	private static final int RECURRENCE_FINITE = 1;
	private static final int RECURRENCE_NONE = 2;

	private final Activity activity;
	private volatile BillingClient client;
	private final Map<String, ProductDetails> products = new ConcurrentHashMap<>();

	// UI thread only, where Play Billing answers calls made from it
	private String flowProductId = "";
	private final Map<String, Integer> productTypes = new HashMap<>();
	// Purchase updates and queries reach the engine in arrival order. An update of a product
	// with unknown type waits for a subscription lookup, and what came after waits behind it.
	private final ArrayDeque<Notification> notifications = new ArrayDeque<>();
	private final Handler handler = new Handler(Looper.getMainLooper());
	private boolean lookingUp = false;

	public BillingWrapper(Activity activity) {
		this.activity = activity;
	}

	public void initialize() {
		activity.runOnUiThread(() -> {
			if (client == null) {
				client = BillingClient.newBuilder(activity)
						.setListener(this)
						.enablePendingPurchases(PendingPurchasesParams.newBuilder()
								.enableOneTimeProducts()
								.enablePrepaidPlans()
								.build())
						.enableAutoServiceReconnection()
						.build();
			}

			if (client.isReady()) {
				nativeOnInitialized(BillingClient.BillingResponseCode.OK, "");
				return;
			}
			// the running connection answers this call too
			if (client.getConnectionState() == BillingClient.ConnectionState.CONNECTING) return;

			client.startConnection(new BillingClientStateListener() {
				@Override
				public void onBillingSetupFinished(@NonNull BillingResult result) {
					nativeOnInitialized(result.getResponseCode(), text(result.getDebugMessage()));
				}

				@Override
				public void onBillingServiceDisconnected() {
					nativeOnDisconnected();
				}
			});
		});
	}

	public boolean isReady() {
		BillingClient current = client;
		return current != null && current.isReady();
	}

	public void queryProducts(String[] productIds, int type) {
		activity.runOnUiThread(() -> {
			if (client == null) {
				nativeOnProductsQueried(BillingClient.BillingResponseCode.SERVICE_DISCONNECTED, NOT_INITIALIZED, "[]");
				return;
			}

			List<QueryProductDetailsParams.Product> productList = new ArrayList<>();
			for (String productId : productIds) {
				productList.add(QueryProductDetailsParams.Product.newBuilder()
						.setProductId(productId)
						.setProductType(productType(type))
						.build());
			}

			client.queryProductDetailsAsync(
					QueryProductDetailsParams.newBuilder().setProductList(productList).build(),
					(result, queried) -> {
						JSONArray json = new JSONArray();
						List<ProductDetails> found = queried.getProductDetailsList();
						for (ProductDetails details : (found != null) ? found : new ArrayList<ProductDetails>()) {
							products.put(details.getProductId(), details);
							productTypes.put(details.getProductId(), typeOf(details));
							try {
								json.put(productJson(details));
							} catch (JSONException e) {
								Log.e(TAG, "Cannot read product " + details.getProductId() + ": " + e.getMessage());
							}
						}

						String message = text(result.getDebugMessage());
						List<UnfetchedProduct> unfetched = queried.getUnfetchedProductList();
						if (unfetched != null && !unfetched.isEmpty()) {
							StringBuilder missing = new StringBuilder(message.isEmpty() ? "" : message + ". ");
							missing.append("Not available:");
							for (UnfetchedProduct product : unfetched) {
								missing.append(' ').append(product.getProductId());
							}
							message = missing.toString();
						}

						nativeOnProductsQueried(result.getResponseCode(), message, json.toString());
					});
		});
	}

	public void purchase(String productId, String offerToken, String oldPurchaseToken, String oldProductId,
			int replacementMode, String accountId, String profileId) {
		activity.runOnUiThread(() -> {
			if (client == null) {
				nativeOnPurchaseFailed(productId, BillingClient.BillingResponseCode.SERVICE_DISCONNECTED, NOT_INITIALIZED);
				return;
			}
			ProductDetails details = products.get(productId);
			if (details == null) {
				nativeOnPurchaseFailed(productId, BillingClient.BillingResponseCode.ITEM_UNAVAILABLE, "Query the product with InAppPurchase.queryProducts first");
				return;
			}

			BillingFlowParams.ProductDetailsParams.Builder productParams =
					BillingFlowParams.ProductDetailsParams.newBuilder().setProductDetails(details);

			// quantum::SubscriptionReplacementMode starts at WITH_TIME_PRORATION
			final int mode = replacementMode + 1;
			// KEEP_EXISTING takes no offer token
			final boolean keepExisting = !oldPurchaseToken.isEmpty()
					&& mode == BillingFlowParams.ProductDetailsParams.SubscriptionProductReplacementParams.ReplacementMode.KEEP_EXISTING;

			if (!keepExisting) {
				String token = offerToken;
				if (token.isEmpty() && BillingClient.ProductType.SUBS.equals(details.getProductType())) {
					ProductDetails.SubscriptionOfferDetails offer = defaultSubscriptionOffer(details);
					if (offer != null) token = text(offer.getOfferToken());
				}
				if (!token.isEmpty()) {
					productParams.setOfferToken(token);
				}
			}

			BillingFlowParams.Builder flowParams = BillingFlowParams.newBuilder();
			if (!oldPurchaseToken.isEmpty()) {
				productParams.setSubscriptionProductReplacementParams(
						BillingFlowParams.ProductDetailsParams.SubscriptionProductReplacementParams.newBuilder()
								.setOldProductId(oldProductId)
								.setReplacementMode(mode)
								.build());
				flowParams.setSubscriptionUpdateParams(BillingFlowParams.SubscriptionUpdateParams.newBuilder()
						.setOldPurchaseToken(oldPurchaseToken)
						.build());
			}

			List<BillingFlowParams.ProductDetailsParams> productParamsList = new ArrayList<>();
			productParamsList.add(productParams.build());
			flowParams.setProductDetailsParamsList(productParamsList);
			if (!accountId.isEmpty()) flowParams.setObfuscatedAccountId(accountId);
			if (!profileId.isEmpty()) flowParams.setObfuscatedProfileId(profileId);

			flowProductId = productId;
			// a flow that cannot start also reports to onPurchasesUpdated
			client.launchBillingFlow(activity, flowParams.build());
		});
	}

	@Override
	public void onPurchasesUpdated(@NonNull BillingResult result, List<Purchase> purchases) {
		String productId = flowProductId;
		flowProductId = "";

		if (result.getResponseCode() != BillingClient.BillingResponseCode.OK) {
			nativeOnPurchaseFailed(productId, result.getResponseCode(), text(result.getDebugMessage()));
			return;
		}
		if (purchases == null) return;

		for (Purchase purchase : purchases) {
			notifications.add(new Notification(purchase, null));
		}
		flushNotifications();
	}

	public void acknowledgePurchase(String purchaseToken) {
		activity.runOnUiThread(() -> {
			if (client == null) {
				nativeOnPurchaseAcknowledged(purchaseToken, BillingClient.BillingResponseCode.SERVICE_DISCONNECTED, NOT_INITIALIZED);
				return;
			}
			client.acknowledgePurchase(
					AcknowledgePurchaseParams.newBuilder().setPurchaseToken(purchaseToken).build(),
					result -> nativeOnPurchaseAcknowledged(purchaseToken, result.getResponseCode(), text(result.getDebugMessage())));
		});
	}

	public void consumePurchase(String purchaseToken) {
		activity.runOnUiThread(() -> {
			if (client == null) {
				nativeOnPurchaseConsumed(purchaseToken, BillingClient.BillingResponseCode.SERVICE_DISCONNECTED, NOT_INITIALIZED);
				return;
			}
			client.consumeAsync(
					ConsumeParams.newBuilder().setPurchaseToken(purchaseToken).build(),
					(result, token) -> nativeOnPurchaseConsumed(purchaseToken, result.getResponseCode(), text(result.getDebugMessage())));
		});
	}

	public void queryPurchases(int type) {
		activity.runOnUiThread(() -> {
			if (client == null) {
				nativeOnPurchasesQueried(type, BillingClient.BillingResponseCode.SERVICE_DISCONNECTED, NOT_INITIALIZED, "[]");
				return;
			}

			client.queryPurchasesAsync(purchasesQuery(type), (result, purchases) -> {
				if (result.getResponseCode() == BillingClient.BillingResponseCode.OK) {
					learnTypes(purchases, type);
				}
				JSONArray json = new JSONArray();
				for (Purchase purchase : purchases) {
					try {
						json.put(purchaseJson(purchase, type));
					} catch (JSONException e) {
						Log.e(TAG, "Cannot read purchase " + purchase.getOrderId() + ": " + e.getMessage());
					}
				}
				final String message = text(result.getDebugMessage());
				notifications.add(new Notification(null,
						() -> nativeOnPurchasesQueried(type, result.getResponseCode(), message, json.toString())));
				flushNotifications();
			});
		});
	}

	public void openSubscriptionManagement(String productId) {
		activity.runOnUiThread(() -> {
			String url = "https://play.google.com/store/account/subscriptions";
			if (!productId.isEmpty()) {
				url += "?sku=" + Uri.encode(productId) + "&package=" + Uri.encode(activity.getPackageName());
			}
			try {
				activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
			} catch (ActivityNotFoundException e) {
				Log.e(TAG, "Cannot open the subscription center: " + e.getMessage());
			}
		});
	}

	public void showInAppMessages() {
		activity.runOnUiThread(() -> {
			if (client == null) return;
			InAppMessageParams params = InAppMessageParams.newBuilder()
					.addInAppMessageCategoryToShow(InAppMessageParams.InAppMessageCategoryId.TRANSACTIONAL)
					.build();
			client.showInAppMessages(activity, params, result -> {
				// the user fixed a payment, so the subscription state changed
				if (result.getResponseCode() == InAppMessageResult.InAppMessageResponseCode.SUBSCRIPTION_STATUS_UPDATED) {
					queryPurchases(TYPE_SUBS);
				}
			});
		});
	}

	@Override
	public void onActivityDestroy() {
		handler.removeCallbacksAndMessages(null);
		if (client != null) {
			client.endConnection();
			client = null;
		}
	}

	private QueryPurchasesParams purchasesQuery(int type) {
		QueryPurchasesParams.Builder params = QueryPurchasesParams.newBuilder().setProductType(productType(type));
		// on hold subscriptions come back as suspended
		if (type == TYPE_SUBS && client.isFeatureSupported(BillingClient.FeatureType.INCLUDE_SUSPENDED_SUBSCRIPTIONS).getResponseCode()
				== BillingClient.BillingResponseCode.OK) {
			params.includeSuspendedSubscriptions(true);
		}
		return params.build();
	}

	private void learnTypes(List<Purchase> purchases, int type) {
		for (Purchase purchase : purchases) {
			for (String productId : purchase.getProducts()) {
				productTypes.put(productId, type);
			}
		}
	}

	private int knownType(Purchase purchase) {
		Integer type = productTypes.get(firstProduct(purchase));
		return (type != null) ? type : TYPE_UNKNOWN;
	}

	private void flushNotifications() {
		while (!notifications.isEmpty()) {
			Notification next = notifications.peek();
			if (next.purchase != null) {
				if (next.type == TYPE_UNKNOWN) next.type = knownType(next.purchase);
				if (next.type == TYPE_UNKNOWN) {
					lookUpSubscriptions();
					return;
				}
				notifyPurchaseUpdated(next.purchase, next.type);
			} else {
				next.send.run();
			}
			notifications.poll();
		}
	}

	// For products the app never queried, bought outside it like a redeemed promo code
	private void lookUpSubscriptions() {
		if (lookingUp || client == null) return;
		lookingUp = true;

		// purchases that already exist for the lookup to see
		Set<String> seen = new HashSet<>();
		for (Notification notification : notifications) {
			if (notification.purchase != null) seen.add(notification.purchase.getPurchaseToken());
		}

		client.queryPurchasesAsync(purchasesQuery(TYPE_SUBS), (result, subscriptions) -> {
			lookingUp = false;
			if (result.getResponseCode() != BillingClient.BillingResponseCode.OK) {
				handler.postDelayed(this::flushNotifications, LOOKUP_RETRY_MS);
				return;
			}
			learnTypes(subscriptions, TYPE_SUBS);
			// seen by the lookup but not among the subscriptions
			for (Notification notification : notifications) {
				if (notification.purchase != null && notification.type == TYPE_UNKNOWN && knownType(notification.purchase) == TYPE_UNKNOWN
						&& seen.contains(notification.purchase.getPurchaseToken())) {
					notification.type = TYPE_INAPP;
				}
			}
			flushNotifications();
		});
	}

	private static String productType(int type) {
		return (type == TYPE_SUBS) ? BillingClient.ProductType.SUBS : BillingClient.ProductType.INAPP;
	}

	private static String text(String value) {
		return (value != null) ? value : "";
	}

	private static int typeOf(ProductDetails details) {
		return BillingClient.ProductType.SUBS.equals(details.getProductType()) ? TYPE_SUBS : TYPE_INAPP;
	}

	// A purchase update, or a ready notification queued behind one
	private static class Notification {
		final Purchase purchase;
		final Runnable send;
		int type = TYPE_UNKNOWN;

		Notification(Purchase purchase, Runnable send) {
			this.purchase = purchase;
			this.send = send;
		}
	}

	private static String firstProduct(Purchase purchase) {
		List<String> productIds = purchase.getProducts();
		return productIds.isEmpty() ? "" : productIds.get(0);
	}

	private static void notifyPurchaseUpdated(Purchase purchase, int type) {
		try {
			nativeOnPurchaseUpdated(purchaseJson(purchase, type).toString());
		} catch (JSONException e) {
			Log.e(TAG, "Cannot read purchase " + purchase.getOrderId() + ": " + e.getMessage());
		}
	}

	// The plan a purchase without an offer token buys: the first base plan, else the first offer
	private static ProductDetails.SubscriptionOfferDetails defaultSubscriptionOffer(ProductDetails details) {
		List<ProductDetails.SubscriptionOfferDetails> offers = details.getSubscriptionOfferDetails();
		if (offers == null || offers.isEmpty()) return null;
		for (ProductDetails.SubscriptionOfferDetails offer : offers) {
			if (offer.getOfferId() == null) return offer;
		}
		return offers.get(0);
	}

	private static JSONArray stringArray(List<String> values) {
		JSONArray array = new JSONArray();
		if (values != null) {
			for (String value : values) array.put(value);
		}
		return array;
	}

	private static JSONObject phaseJson(String price, long priceMicros, String currencyCode, String billingPeriod,
			int billingCycleCount, int recurrenceMode) throws JSONException {
		JSONObject phase = new JSONObject();
		phase.put("price", text(price));
		phase.put("priceMicros", priceMicros);
		phase.put("currencyCode", text(currencyCode));
		phase.put("billingPeriod", text(billingPeriod));
		phase.put("billingCycleCount", billingCycleCount);
		phase.put("recurrenceMode", recurrenceMode);
		return phase;
	}

	private static JSONObject productJson(ProductDetails details) throws JSONException {
		JSONObject json = new JSONObject();
		json.put("productId", details.getProductId());
		json.put("type", typeOf(details));
		json.put("title", text(details.getTitle()));
		json.put("name", text(details.getName()));
		json.put("description", text(details.getDescription()));

		JSONArray offers = new JSONArray();

		ProductDetails.OneTimePurchaseOfferDetails oneTime = details.getOneTimePurchaseOfferDetails();
		if (oneTime != null) {
			json.put("price", text(oneTime.getFormattedPrice()));
			json.put("priceMicros", oneTime.getPriceAmountMicros());
			json.put("currencyCode", text(oneTime.getPriceCurrencyCode()));
		}

		List<ProductDetails.OneTimePurchaseOfferDetails> oneTimeOffers = details.getOneTimePurchaseOfferDetailsList();
		if (oneTimeOffers != null) {
			for (ProductDetails.OneTimePurchaseOfferDetails offer : oneTimeOffers) {
				JSONObject offerJson = new JSONObject();
				offerJson.put("offerToken", text(offer.getOfferToken()));
				offerJson.put("offerId", text(offer.getOfferId()));
				offerJson.put("basePlanId", text(offer.getPurchaseOptionId()));
				offerJson.put("tags", stringArray(offer.getOfferTags()));
				JSONArray phases = new JSONArray();
				phases.put(phaseJson(offer.getFormattedPrice(), offer.getPriceAmountMicros(), offer.getPriceCurrencyCode(),
						"", 0, RECURRENCE_NONE));
				offerJson.put("pricingPhases", phases);
				offers.put(offerJson);
			}
		}

		List<ProductDetails.SubscriptionOfferDetails> subscriptionOffers = details.getSubscriptionOfferDetails();
		if (subscriptionOffers != null) {
			for (ProductDetails.SubscriptionOfferDetails offer : subscriptionOffers) {
				JSONObject offerJson = new JSONObject();
				offerJson.put("offerToken", text(offer.getOfferToken()));
				offerJson.put("offerId", text(offer.getOfferId()));
				offerJson.put("basePlanId", text(offer.getBasePlanId()));
				offerJson.put("tags", stringArray(offer.getOfferTags()));

				JSONArray phases = new JSONArray();
				for (ProductDetails.PricingPhase phase : offer.getPricingPhases().getPricingPhaseList()) {
					int recurrence;
					switch (phase.getRecurrenceMode()) {
						case ProductDetails.RecurrenceMode.INFINITE_RECURRING: recurrence = RECURRENCE_INFINITE; break;
						case ProductDetails.RecurrenceMode.FINITE_RECURRING: recurrence = RECURRENCE_FINITE; break;
						default: recurrence = RECURRENCE_NONE; break;
					}
					phases.put(phaseJson(phase.getFormattedPrice(), phase.getPriceAmountMicros(), phase.getPriceCurrencyCode(),
							phase.getBillingPeriod(), phase.getBillingCycleCount(), recurrence));
				}
				offerJson.put("pricingPhases", phases);
				offers.put(offerJson);
			}

			// the recurring price of the plan bought without an offer token
			ProductDetails.SubscriptionOfferDetails defaultOffer = defaultSubscriptionOffer(details);
			if (defaultOffer != null) {
				List<ProductDetails.PricingPhase> defaultPhases = defaultOffer.getPricingPhases().getPricingPhaseList();
				if (!defaultPhases.isEmpty()) {
					ProductDetails.PricingPhase recurring = defaultPhases.get(defaultPhases.size() - 1);
					json.put("price", text(recurring.getFormattedPrice()));
					json.put("priceMicros", recurring.getPriceAmountMicros());
					json.put("currencyCode", text(recurring.getPriceCurrencyCode()));
				}
			}
		}

		json.put("offers", offers);
		return json;
	}

	private static JSONObject purchaseJson(Purchase purchase, int type) throws JSONException {
		JSONObject json = new JSONObject();
		json.put("orderId", text(purchase.getOrderId()));
		json.put("productIds", stringArray(purchase.getProducts()));
		json.put("productType", type);
		json.put("purchaseToken", text(purchase.getPurchaseToken()));
		json.put("purchaseTime", purchase.getPurchaseTime());
		// same order as quantum::PurchaseState
		json.put("state", purchase.getPurchaseState());
		json.put("quantity", purchase.getQuantity());
		json.put("acknowledged", purchase.isAcknowledged());
		json.put("autoRenewing", purchase.isAutoRenewing());
		json.put("suspended", purchase.isSuspended());
		json.put("packageName", text(purchase.getPackageName()));
		AccountIdentifiers accountIdentifiers = purchase.getAccountIdentifiers();
		if (accountIdentifiers != null) {
			json.put("obfuscatedAccountId", text(accountIdentifiers.getObfuscatedAccountId()));
			json.put("obfuscatedProfileId", text(accountIdentifiers.getObfuscatedProfileId()));
		}
		json.put("originalJson", text(purchase.getOriginalJson()));
		json.put("signature", text(purchase.getSignature()));
		return json;
	}

	// Registered by AndroidServices.cpp
	private static native void nativeOnInitialized(int responseCode, String message);
	private static native void nativeOnDisconnected();
	private static native void nativeOnProductsQueried(int responseCode, String message, String productsJson);
	private static native void nativeOnPurchaseUpdated(String purchaseJson);
	private static native void nativeOnPurchaseFailed(String productId, int responseCode, String message);
	private static native void nativeOnPurchasesQueried(int productType, int responseCode, String message, String purchasesJson);
	private static native void nativeOnPurchaseAcknowledged(String purchaseToken, int responseCode, String message);
	private static native void nativeOnPurchaseConsumed(String purchaseToken, int responseCode, String message);
}
