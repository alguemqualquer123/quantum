// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "InAppPurchase.h"

#include "Log.h"
#include "System.h"
#include "json.hpp"
#include <algorithm>

using namespace quantum;

namespace {
    const char* notAvailableMessage = "In-app purchases are not available on this platform or build";

    void warnNotAvailable(){
        static bool warned = false;
        if (!warned){
            Log::warn("%s. Android and iOS exports have them when Google Play Billing or App Store Purchases is enabled in the project settings.", notAvailableMessage);
            warned = true;
        }
    }

    InAppPurchaseBackend* getBackend(){
        return System::instance().getInAppPurchaseBackend();
    }

    using Json = nlohmann::json;

    std::string jsonString(const Json& json, const char* key){
        auto it = json.find(key);
        return (it != json.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

    long long jsonInteger(const Json& json, const char* key){
        auto it = json.find(key);
        return (it != json.end() && it->is_number_integer()) ? it->get<long long>() : 0;
    }

    bool jsonBool(const Json& json, const char* key){
        auto it = json.find(key);
        return it != json.end() && it->is_boolean() && it->get<bool>();
    }

    std::vector<std::string> jsonStrings(const Json& json, const char* key){
        std::vector<std::string> values;
        auto it = json.find(key);
        if (it != json.end() && it->is_array()){
            for (const Json& item : *it){
                if (item.is_string()) values.push_back(item.get<std::string>());
            }
        }
        return values;
    }

    ProductDetails parseProduct(const Json& json){
        ProductDetails product;
        product.productId = jsonString(json, "productId");
        product.type = static_cast<ProductType>(jsonInteger(json, "type"));
        product.title = jsonString(json, "title");
        product.name = jsonString(json, "name");
        product.description = jsonString(json, "description");
        product.price = jsonString(json, "price");
        product.priceMicros = jsonInteger(json, "priceMicros");
        product.currencyCode = jsonString(json, "currencyCode");

        auto offers = json.find("offers");
        if (offers != json.end() && offers->is_array()){
            for (const Json& offerJson : *offers){
                ProductOffer offer;
                offer.offerToken = jsonString(offerJson, "offerToken");
                offer.offerId = jsonString(offerJson, "offerId");
                offer.basePlanId = jsonString(offerJson, "basePlanId");
                offer.tags = jsonStrings(offerJson, "tags");

                auto phases = offerJson.find("pricingPhases");
                if (phases != offerJson.end() && phases->is_array()){
                    for (const Json& phaseJson : *phases){
                        PricingPhase phase;
                        phase.price = jsonString(phaseJson, "price");
                        phase.priceMicros = jsonInteger(phaseJson, "priceMicros");
                        phase.currencyCode = jsonString(phaseJson, "currencyCode");
                        phase.billingPeriod = jsonString(phaseJson, "billingPeriod");
                        phase.billingCycleCount = static_cast<int>(jsonInteger(phaseJson, "billingCycleCount"));
                        phase.recurrenceMode = static_cast<RecurrenceMode>(jsonInteger(phaseJson, "recurrenceMode"));
                        offer.pricingPhases.push_back(phase);
                    }
                }
                product.offers.push_back(offer);
            }
        }

        return product;
    }

    PurchaseDetails parsePurchase(const Json& json){
        PurchaseDetails purchase;
        purchase.orderId = jsonString(json, "orderId");
        purchase.productIds = jsonStrings(json, "productIds");
        purchase.productId = purchase.productIds.empty() ? "" : purchase.productIds[0];
        purchase.productType = static_cast<ProductType>(jsonInteger(json, "productType"));
        purchase.purchaseToken = jsonString(json, "purchaseToken");
        purchase.purchaseTime = jsonInteger(json, "purchaseTime");
        purchase.state = static_cast<PurchaseState>(jsonInteger(json, "state"));
        purchase.quantity = static_cast<int>(jsonInteger(json, "quantity"));
        purchase.acknowledged = jsonBool(json, "acknowledged");
        purchase.autoRenewing = jsonBool(json, "autoRenewing");
        purchase.suspended = jsonBool(json, "suspended");
        purchase.packageName = jsonString(json, "packageName");
        purchase.obfuscatedAccountId = jsonString(json, "obfuscatedAccountId");
        purchase.obfuscatedProfileId = jsonString(json, "obfuscatedProfileId");
        purchase.originalJson = jsonString(json, "originalJson");
        purchase.signature = jsonString(json, "signature");
        return purchase;
    }

    template<typename T>
    std::vector<T> parseList(const std::string& text, T (*parse)(const Json&)){
        std::vector<T> list;
        Json json = Json::parse(text, nullptr, false);
        if (json.is_discarded()){
            Log::error("The store sent unreadable in-app purchase data");
        }else if (json.is_object()){
            list.push_back(parse(json));
        }else if (json.is_array()){
            for (const Json& item : json){
                if (item.is_object()) list.push_back(parse(item));
            }
        }
        return list;
    }
}

std::map<std::string, ProductDetails> InAppPurchase::products;
std::vector<PurchaseDetails> InAppPurchase::purchases;
std::string InAppPurchase::obfuscatedAccountId;
std::string InAppPurchase::obfuscatedProfileId;

std::mutex InAppPurchase::eventMutex;
std::vector<std::function<void()>> InAppPurchase::pendingEvents;

FunctionSubscribe<void(BillingResponse, std::string)> InAppPurchase::onInitialized;
FunctionSubscribe<void()> InAppPurchase::onDisconnected;
FunctionSubscribe<void(BillingResponse, std::string)> InAppPurchase::onProductsQueried;
FunctionSubscribe<void(PurchaseDetails)> InAppPurchase::onPurchaseUpdated;
FunctionSubscribe<void(std::string, BillingResponse, std::string)> InAppPurchase::onPurchaseFailed;
FunctionSubscribe<void(ProductType, BillingResponse, std::string)> InAppPurchase::onPurchasesQueried;
FunctionSubscribe<void(std::string, BillingResponse, std::string)> InAppPurchase::onPurchaseAcknowledged;
FunctionSubscribe<void(std::string, BillingResponse, std::string)> InAppPurchase::onPurchaseConsumed;

void InAppPurchase::postEvent(std::function<void()> event){
    std::lock_guard<std::mutex> lock(eventMutex);
    pendingEvents.push_back(std::move(event));
}

void InAppPurchase::dispatchEvents(){
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

void InAppPurchase::clearSubscriptions(){
    onInitialized.clear();
    onDisconnected.clear();
    onProductsQueried.clear();
    onPurchaseUpdated.clear();
    onPurchaseFailed.clear();
    onPurchasesQueried.clear();
    onPurchaseAcknowledged.clear();
    onPurchaseConsumed.clear();
}

void InAppPurchase::removeSubscriptionsByTag(const std::string& substring){
    onInitialized.removeByTagSubstring(substring);
    onDisconnected.removeByTagSubstring(substring);
    onProductsQueried.removeByTagSubstring(substring);
    onPurchaseUpdated.removeByTagSubstring(substring);
    onPurchaseFailed.removeByTagSubstring(substring);
    onPurchasesQueried.removeByTagSubstring(substring);
    onPurchaseAcknowledged.removeByTagSubstring(substring);
    onPurchaseConsumed.removeByTagSubstring(substring);
}

void InAppPurchase::reset(){
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        pendingEvents.clear();
    }

    products.clear();
    purchases.clear();
    obfuscatedAccountId.clear();
    obfuscatedProfileId.clear();
}

void InAppPurchase::initialize(){
    if (InAppPurchaseBackend* backend = getBackend()){
        backend->initialize();
    }else{
        warnNotAvailable();
        postEvent([](){ onInitialized.call(BillingResponse::BILLING_UNAVAILABLE, notAvailableMessage); });
    }
}

bool InAppPurchase::isReady(){
    InAppPurchaseBackend* backend = getBackend();
    return backend && backend->isReady();
}

void InAppPurchase::queryProducts(const std::vector<std::string>& productIds, ProductType type){
    if (productIds.empty()){
        postEvent([](){ onProductsQueried.call(BillingResponse::DEVELOPER_ERROR, "No product ids to query"); });
        return;
    }

    if (InAppPurchaseBackend* backend = getBackend()){
        backend->queryProducts(productIds, type);
    }else{
        warnNotAvailable();
        postEvent([](){ onProductsQueried.call(BillingResponse::BILLING_UNAVAILABLE, notAvailableMessage); });
    }
}

bool InAppPurchase::hasProduct(const std::string& productId){
    return products.find(productId) != products.end();
}

ProductDetails InAppPurchase::getProduct(const std::string& productId){
    auto it = products.find(productId);
    if (it == products.end()){
        return ProductDetails();
    }
    return it->second;
}

std::vector<ProductDetails> InAppPurchase::getProducts(){
    std::vector<ProductDetails> list;
    for (const auto& entry : products){
        list.push_back(entry.second);
    }
    return list;
}

void InAppPurchase::launchPurchase(const PurchaseParams& params){
    if (InAppPurchaseBackend* backend = getBackend()){
        backend->purchase(params);
    }else{
        warnNotAvailable();
        const std::string productId = params.productId;
        postEvent([productId](){ onPurchaseFailed.call(productId, BillingResponse::BILLING_UNAVAILABLE, notAvailableMessage); });
    }
}

void InAppPurchase::purchase(const std::string& productId, const std::string& offerToken){
    PurchaseParams params;
    params.productId = productId;
    params.offerToken = offerToken;
    params.obfuscatedAccountId = obfuscatedAccountId;
    params.obfuscatedProfileId = obfuscatedProfileId;

    launchPurchase(params);
}

void InAppPurchase::changeSubscription(const std::string& productId, const std::string& offerToken, const std::string& oldPurchaseToken, SubscriptionReplacementMode mode){
    auto old = std::find_if(purchases.begin(), purchases.end(), [&](const PurchaseDetails& purchase){
        return purchase.purchaseToken == oldPurchaseToken;
    });
    if (old == purchases.end()){
        postEvent([productId](){
            onPurchaseFailed.call(productId, BillingResponse::DEVELOPER_ERROR, "The subscription to replace is not owned. Query subscription purchases first");
        });
        return;
    }

    PurchaseParams params;
    params.productId = productId;
    params.offerToken = offerToken;
    params.oldPurchaseToken = oldPurchaseToken;
    params.oldProductId = old->productId;
    params.replacementMode = mode;
    params.obfuscatedAccountId = obfuscatedAccountId;
    params.obfuscatedProfileId = obfuscatedProfileId;

    launchPurchase(params);
}

void InAppPurchase::acknowledgePurchase(const std::string& purchaseToken){
    if (InAppPurchaseBackend* backend = getBackend()){
        backend->acknowledgePurchase(purchaseToken);
    }else{
        warnNotAvailable();
        postEvent([purchaseToken](){ onPurchaseAcknowledged.call(purchaseToken, BillingResponse::BILLING_UNAVAILABLE, notAvailableMessage); });
    }
}

void InAppPurchase::consumePurchase(const std::string& purchaseToken){
    if (InAppPurchaseBackend* backend = getBackend()){
        backend->consumePurchase(purchaseToken);
    }else{
        warnNotAvailable();
        postEvent([purchaseToken](){ onPurchaseConsumed.call(purchaseToken, BillingResponse::BILLING_UNAVAILABLE, notAvailableMessage); });
    }
}

void InAppPurchase::queryPurchases(ProductType type){
    if (InAppPurchaseBackend* backend = getBackend()){
        backend->queryPurchases(type);
    }else{
        warnNotAvailable();
        postEvent([type](){ onPurchasesQueried.call(type, BillingResponse::BILLING_UNAVAILABLE, notAvailableMessage); });
    }
}

void InAppPurchase::restorePurchases(){
    if (InAppPurchaseBackend* backend = getBackend()){
        backend->restorePurchases();
    }else{
        warnNotAvailable();
        postEvent([](){
            onPurchasesQueried.call(ProductType::INAPP, BillingResponse::BILLING_UNAVAILABLE, notAvailableMessage);
            onPurchasesQueried.call(ProductType::SUBS, BillingResponse::BILLING_UNAVAILABLE, notAvailableMessage);
        });
    }
}

std::vector<PurchaseDetails> InAppPurchase::getPurchases(){
    return purchases;
}

bool InAppPurchase::isPurchased(const std::string& productId){
    for (const PurchaseDetails& purchase : purchases){
        if (purchase.state != PurchaseState::PURCHASED || purchase.suspended)
            continue;
        if (std::find(purchase.productIds.begin(), purchase.productIds.end(), productId) != purchase.productIds.end())
            return true;
    }
    return false;
}

void InAppPurchase::setObfuscatedAccountId(const std::string& accountId){
    obfuscatedAccountId = accountId;
}

void InAppPurchase::setObfuscatedProfileId(const std::string& profileId){
    obfuscatedProfileId = profileId;
}

void InAppPurchase::openSubscriptionManagement(const std::string& productId){
    if (InAppPurchaseBackend* backend = getBackend()){
        backend->openSubscriptionManagement(productId);
    }else{
        warnNotAvailable();
    }
}

void InAppPurchase::showInAppMessages(){
    if (InAppPurchaseBackend* backend = getBackend()){
        backend->showInAppMessages();
    }else{
        warnNotAvailable();
    }
}

void InAppPurchase::storePurchase(const PurchaseDetails& purchase){
    for (PurchaseDetails& stored : purchases){
        if (stored.purchaseToken == purchase.purchaseToken){
            stored = purchase;
            return;
        }
    }
    purchases.push_back(purchase);
}

void InAppPurchase::systemInitialized(BillingResponse response, const std::string& message){
    postEvent([response, message](){ onInitialized.call(response, message); });
}

void InAppPurchase::systemDisconnected(){
    postEvent([](){ onDisconnected.call(); });
}

void InAppPurchase::systemProductsQueried(BillingResponse response, const std::string& message, const std::vector<ProductDetails>& queried){
    postEvent([response, message, queried](){
        for (const ProductDetails& product : queried){
            products[product.productId] = product;
        }
        onProductsQueried.call(response, message);
    });
}

void InAppPurchase::systemPurchaseUpdated(const PurchaseDetails& purchase){
    postEvent([purchase](){
        // an iOS purchase waiting for approval has no token yet
        if (!purchase.purchaseToken.empty()){
            storePurchase(purchase);
        }
        onPurchaseUpdated.call(purchase);
    });
}

void InAppPurchase::systemPurchaseFailed(const std::string& productId, BillingResponse response, const std::string& message){
    postEvent([productId, response, message](){ onPurchaseFailed.call(productId, response, message); });
}

void InAppPurchase::systemPurchasesQueried(ProductType type, BillingResponse response, const std::string& message, const std::vector<PurchaseDetails>& owned){
    postEvent([type, response, message, owned](){
        if (response == BillingResponse::OK){
            // the store's answer replaces this type: refunded and expired purchases go away
            purchases.erase(std::remove_if(purchases.begin(), purchases.end(), [type](const PurchaseDetails& purchase){
                return purchase.productType == type;
            }), purchases.end());

            std::vector<PurchaseDetails> restored = owned;
            for (PurchaseDetails& purchase : restored){
                purchase.productType = type;
                purchase.restored = true;
                storePurchase(purchase);
            }
            for (const PurchaseDetails& purchase : restored){
                onPurchaseUpdated.call(purchase);
            }
        }
        onPurchasesQueried.call(type, response, message);
    });
}

void InAppPurchase::systemPurchaseAcknowledged(const std::string& purchaseToken, BillingResponse response, const std::string& message){
    postEvent([purchaseToken, response, message](){
        if (response == BillingResponse::OK){
            for (PurchaseDetails& purchase : purchases){
                if (purchase.purchaseToken == purchaseToken){
                    purchase.acknowledged = true;
                }
            }
        }
        onPurchaseAcknowledged.call(purchaseToken, response, message);
    });
}

void InAppPurchase::systemPurchaseConsumed(const std::string& purchaseToken, BillingResponse response, const std::string& message){
    postEvent([purchaseToken, response, message](){
        if (response == BillingResponse::OK){
            // a consumed product is no longer owned
            purchases.erase(std::remove_if(purchases.begin(), purchases.end(), [&purchaseToken](const PurchaseDetails& purchase){
                return purchase.purchaseToken == purchaseToken;
            }), purchases.end());
        }
        onPurchaseConsumed.call(purchaseToken, response, message);
    });
}

std::vector<ProductDetails> InAppPurchase::productsFromJson(const std::string& json){
    return parseList(json, parseProduct);
}

std::vector<PurchaseDetails> InAppPurchase::purchasesFromJson(const std::string& json){
    return parseList(json, parsePurchase);
}
