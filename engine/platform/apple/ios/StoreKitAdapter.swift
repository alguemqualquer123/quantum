// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

import Foundation
import StoreKit
import UIKit

// quantum::ProductType
private let typeInApp: Int32 = 0
private let typeSubs: Int32 = 1

// quantum::PurchaseState
private let stateUnspecified = 0
private let statePurchased = 1
private let statePending = 2

// quantum::RecurrenceMode
private let recurrenceInfinite = 0
private let recurrenceFinite = 1
private let recurrenceNone = 2

// quantum::BillingResponse
private enum Response: Int32 {
    case serviceDisconnected = -1
    case ok = 0
    case userCanceled = 1
    case billingUnavailable = 3
    case itemUnavailable = 4
    case developerError = 5
    case fatalError = 6
    case itemNotOwned = 8
    case networkError = 12
}

private let notInitialized = "Call InAppPurchase.initialize first"

@objc @implementation extension StoreKitAdapter {
    // set and read on the engine thread
    private final var started = false

    // Store keeps its state on the main actor
    func initializeStore() {
        started = true
        Task { @MainActor in Store.shared.initialize() }
    }

    func isReady() -> Bool {
        return started
    }

    func queryProducts(_ productIds: [String], type: Int32) {
        Task { @MainActor in Store.shared.queryProducts(productIds, type: type) }
    }

    func purchase(_ productId: String, accountToken: String) {
        Task { @MainActor in Store.shared.purchase(productId, accountToken: accountToken) }
    }

    func finishPurchase(_ purchaseToken: String, consume: Bool) {
        Task { @MainActor in Store.shared.finishPurchase(purchaseToken, consume: consume) }
    }

    func queryPurchases(_ type: Int32) {
        Task { @MainActor in Store.shared.queryPurchases(type) }
    }

    func restorePurchases() {
        Task { @MainActor in Store.shared.restorePurchases() }
    }

    func openSubscriptionManagement(_ productId: String) {
        Task { @MainActor in Store.shared.openSubscriptionManagement(productId) }
    }
}

@MainActor
private final class Store {
    static let shared = Store()

    private var initialized = false
    // purchases need their product queried
    private var products: [String: Product] = [:]
    // a purchase reported twice is consumed once
    private var consumed: Set<UInt64> = []

    func initialize() {
        if !initialized {
            initialized = true
            // also gets the transactions left unfinished, once per launch
            Task {
                for await result in Transaction.updates {
                    await transactionUpdated(result)
                }
            }
        }
        QuantumStoreKitInitialized(Response.ok.rawValue, "")
    }

    func queryProducts(_ productIds: [String], type: Int32) {
        guard initialized else {
            QuantumStoreKitProductsQueried(Response.serviceDisconnected.rawValue, notInitialized, "[]")
            return
        }

        Task {
            let found: [Product]
            do {
                found = try await Product.products(for: productIds)
            } catch {
                QuantumStoreKitProductsQueried(response(for: error).rawValue, error.localizedDescription, "[]")
                return
            }

            // like on Google Play, a query finds the products of its type only
            var list: [[String: Any]] = []
            var otherType: [String] = []
            for product in found {
                if productType(product.type) != type {
                    otherType.append(product.id)
                    continue
                }
                products[product.id] = product
                list.append(await productJson(product))
            }

            var notes: [String] = []
            let missing = productIds.filter { id in !found.contains { $0.id == id } }
            if !missing.isEmpty {
                notes.append("Not available: " + missing.joined(separator: " "))
            }
            if !otherType.isEmpty {
                notes.append((type == typeSubs ? "Not subscriptions: " : "Subscriptions, query them as SUBS: ") + otherType.joined(separator: " "))
            }
            QuantumStoreKitProductsQueried(Response.ok.rawValue, notes.joined(separator: ". "), json(list))
        }
    }

    func purchase(_ productId: String, accountToken: String) {
        guard initialized else {
            QuantumStoreKitPurchaseFailed(productId, Response.serviceDisconnected.rawValue, notInitialized)
            return
        }
        guard let product = products[productId] else {
            QuantumStoreKitPurchaseFailed(productId, Response.itemUnavailable.rawValue, "Query the product with InAppPurchase.queryProducts first")
            return
        }
        guard AppStore.canMakePayments else {
            QuantumStoreKitPurchaseFailed(productId, Response.billingUnavailable.rawValue, "This device is not allowed to make payments")
            return
        }

        var options: Set<Product.PurchaseOption> = []
        if let token = UUID(uuidString: accountToken) {
            options.insert(.appAccountToken(token))
        }

        Task {
            do {
                switch try await product.purchase(options: options) {
                case .success(let result):
                    switch result {
                    case .verified(let transaction):
                        await report(transaction, jws: result.jwsRepresentation)
                    case .unverified(_, let error):
                        QuantumStoreKitPurchaseFailed(productId, Response.fatalError.rawValue, "The App Store could not verify the purchase: " + error.localizedDescription)
                    }
                case .pending:
                    // Ask to Buy: the transaction comes as an update once approved
                    QuantumStoreKitPurchaseUpdated(json(pendingJson(product)))
                case .userCancelled:
                    QuantumStoreKitPurchaseFailed(productId, Response.userCanceled.rawValue, "")
                @unknown default:
                    QuantumStoreKitPurchaseFailed(productId, Response.fatalError.rawValue, "Unknown purchase result")
                }
            } catch {
                QuantumStoreKitPurchaseFailed(productId, response(for: error).rawValue, error.localizedDescription)
            }
        }
    }

    func finishPurchase(_ purchaseToken: String, consume: Bool) {
        guard initialized else {
            answerFinish(purchaseToken, consume: consume, .serviceDisconnected, notInitialized)
            return
        }
        guard let originalID = UInt64(purchaseToken) else {
            answerFinish(purchaseToken, consume: consume, .developerError, "Not an App Store purchase token")
            return
        }
        if consume && !consumed.insert(originalID).inserted {
            answerFinish(purchaseToken, consume: consume, .itemNotOwned, "The purchase is already consumed")
            return
        }

        Task {
            // a subscription can have renewals waiting too
            var waiting: [Transaction] = []
            for await result in Transaction.unfinished {
                if case .verified(let transaction) = result, transaction.originalID == originalID {
                    waiting.append(transaction)
                }
            }
            var purchase = waiting.last
            if purchase == nil {
                // finished before, so still owned unless it is a consumable
                for await result in Transaction.currentEntitlements {
                    if case .verified(let transaction) = result, transaction.originalID == originalID {
                        purchase = transaction
                    }
                }
            }

            guard let purchase else {
                if consume {
                    consumed.remove(originalID)
                }
                answerFinish(purchaseToken, consume: consume, .itemNotOwned, "No purchase has this token")
                return
            }
            // refused before finishing anything
            if consume && purchase.productType != .consumable {
                consumed.remove(originalID)
                answerFinish(purchaseToken, consume: consume, .developerError, purchase.productID + " is not a consumable in App Store Connect, so acknowledge it instead")
                return
            }
            for transaction in waiting {
                await transaction.finish()
            }
            answerFinish(purchaseToken, consume: consume, .ok, "")
        }
    }

    func queryPurchases(_ type: Int32) {
        guard initialized else {
            QuantumStoreKitPurchasesQueried(type, Response.serviceDisconnected.rawValue, notInitialized, "[]")
            return
        }

        Task {
            let list = await ownedPurchases(type)
            QuantumStoreKitPurchasesQueried(type, Response.ok.rawValue, "", json(list))
        }
    }

    func restorePurchases() {
        guard initialized else {
            for type in [typeInApp, typeSubs] {
                QuantumStoreKitPurchasesQueried(type, Response.serviceDisconnected.rawValue, notInitialized, "[]")
            }
            return
        }

        Task {
            do {
                // may ask the user to sign in
                try await AppStore.sync()
            } catch {
                for type in [typeInApp, typeSubs] {
                    QuantumStoreKitPurchasesQueried(type, response(for: error).rawValue, error.localizedDescription, "[]")
                }
                return
            }
            for type in [typeInApp, typeSubs] {
                let list = await ownedPurchases(type)
                QuantumStoreKitPurchasesQueried(type, Response.ok.rawValue, "", json(list))
            }
        }
    }

    func openSubscriptionManagement(_ productId: String) {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        guard let scene = scenes.first(where: { $0.activationState == .foregroundActive }) ?? scenes.first else {
            openSubscriptionsPage()
            return
        }
        let group = products[productId]?.subscription?.subscriptionGroupID

        Task {
            do {
                if #available(iOS 17.0, *), let group {
                    try await AppStore.showManageSubscriptions(in: scene, subscriptionGroupID: group)
                } else {
                    try await AppStore.showManageSubscriptions(in: scene)
                }
            } catch {
                warn("Cannot show the subscriptions sheet: " + error.localizedDescription)
                openSubscriptionsPage()
                return
            }
            // the user may have changed or canceled one
            if initialized {
                queryPurchases(typeSubs)
            }
        }
    }

    private func openSubscriptionsPage() {
        if let url = URL(string: "https://apps.apple.com/account/subscriptions") {
            UIApplication.shared.open(url)
        }
    }

    private func transactionUpdated(_ result: VerificationResult<Transaction>) async {
        guard case .verified(let transaction) = result else {
            warn("Skipped a transaction the App Store could not verify")
            return
        }
        // the subscription that replaced it reports the change
        if transaction.isUpgraded {
            await transaction.finish()
            return
        }
        if transaction.productType == .autoRenewable {
            await subscriptionUpdated(transaction, jws: result.jwsRepresentation)
            return
        }
        // refunded or no longer shared by the family
        if transaction.revocationDate != nil {
            await transaction.finish()
            QuantumStoreKitPurchaseUpdated(json(purchaseJson(transaction, jws: result.jwsRepresentation, state: stateUnspecified, acknowledged: true)))
            return
        }
        await report(transaction, jws: result.jwsRepresentation)
    }

    // Reports the subscription as it is now: the update can be a late renewal or a refunded period
    private func subscriptionUpdated(_ transaction: Transaction, jws: String) async {
        // entitled while subscribed or in a billing grace period
        var entitled: (transaction: Transaction, jws: String)?
        for await result in Transaction.currentEntitlements {
            if case .verified(let latest) = result, latest.originalID == transaction.originalID, !latest.isUpgraded {
                entitled = (latest, result.jwsRepresentation)
            }
        }
        var onHold: Product.SubscriptionInfo.Status?
        if entitled == nil, let status = await transaction.subscriptionStatus, status.state == .inBillingRetryPeriod {
            onHold = status
        }

        // nothing left to deliver
        if transaction.revocationDate != nil || (entitled == nil && onHold == nil) {
            await transaction.finish()
        }

        if let entitled {
            await report(entitled.transaction, jws: entitled.jws)
        } else if let onHold, case .verified(let latest) = onHold.transaction {
            // on hold for a payment problem
            QuantumStoreKitPurchaseUpdated(json(purchaseJson(latest, jws: onHold.transaction.jwsRepresentation, state: statePurchased,
                acknowledged: false, autoRenewing: willAutoRenew(onHold), suspended: true)))
        } else {
            // expired or refunded
            QuantumStoreKitPurchaseUpdated(json(purchaseJson(transaction, jws: jws, state: stateUnspecified, acknowledged: true)))
        }
    }

    // A purchase to grant, then acknowledge or consume
    private func report(_ transaction: Transaction, jws: String) async {
        var autoRenewing = false
        var suspended = false
        if transaction.productType == .autoRenewable {
            let status = await transaction.subscriptionStatus
            autoRenewing = status.map(willAutoRenew) ?? true
            suspended = status?.state == .inBillingRetryPeriod
        }
        QuantumStoreKitPurchaseUpdated(json(purchaseJson(transaction, jws: jws, state: statePurchased, acknowledged: false,
            autoRenewing: autoRenewing, suspended: suspended)))
    }

    // What queryPurchases reports. Finished transactions are acknowledged.
    private func ownedPurchases(_ type: Int32) async -> [[String: Any]] {
        var unfinished: Set<UInt64> = []
        var owned: [(transaction: Transaction, jws: String)] = []
        for await result in Transaction.unfinished {
            guard case .verified(let transaction) = result else { continue }
            unfinished.insert(transaction.id)
            // consumables are owned until consumed, and no entitlement lists them
            if type == typeInApp && transaction.productType == .consumable && transaction.revocationDate == nil {
                owned.append((transaction, result.jwsRepresentation))
            }
        }
        for await result in Transaction.currentEntitlements {
            guard case .verified(let transaction) = result else {
                warn("Skipped a purchase the App Store could not verify")
                continue
            }
            if productType(transaction.productType) == type && transaction.productType != .consumable && !transaction.isUpgraded {
                owned.append((transaction, result.jwsRepresentation))
            }
        }

        guard type == typeSubs else {
            return owned.map { purchaseJson($0.transaction, jws: $0.jws, state: statePurchased, acknowledged: !unfinished.contains($0.transaction.id)) }
        }

        let statuses = await subscriptionStatuses()
        var list = owned.map { item in
            let status = statuses[item.transaction.originalID]
            return purchaseJson(item.transaction, jws: item.jws, state: statePurchased, acknowledged: !unfinished.contains(item.transaction.id),
                autoRenewing: status.map(willAutoRenew) ?? true, suspended: status?.state == .inBillingRetryPeriod)
        }
        // on hold for a payment problem, so not an entitlement
        let listed = Set(owned.map { $0.transaction.originalID })
        for (originalID, status) in statuses where status.state == .inBillingRetryPeriod && !listed.contains(originalID) {
            if case .verified(let transaction) = status.transaction {
                list.append(purchaseJson(transaction, jws: status.transaction.jwsRepresentation, state: statePurchased,
                    acknowledged: !unfinished.contains(transaction.id), autoRenewing: willAutoRenew(status), suspended: true))
            }
        }
        return list
    }

    // By original transaction. The groups come from the history, as one on hold is not an entitlement.
    private func subscriptionStatuses() async -> [UInt64: Product.SubscriptionInfo.Status] {
        var groups = Set<String>()
        for await result in Transaction.all {
            if case .verified(let transaction) = result, let group = transaction.subscriptionGroupID {
                groups.insert(group)
            }
        }

        var statuses: [UInt64: Product.SubscriptionInfo.Status] = [:]
        for group in groups {
            for status in (try? await Product.SubscriptionInfo.status(for: group)) ?? [] {
                if case .verified(let transaction) = status.transaction {
                    statuses[transaction.originalID] = status
                }
            }
        }
        return statuses
    }
}

private func productType(_ type: Product.ProductType) -> Int32 {
    return type == .autoRenewable ? typeSubs : typeInApp
}

private func productJson(_ product: Product) async -> [String: Any] {
    let currencyCode = product.priceFormatStyle.currencyCode

    var phases: [[String: Any]] = []
    if let subscription = product.subscription {
        // the App Store applies it by itself to those eligible
        if let intro = subscription.introductoryOffer, await subscription.isEligibleForIntroOffer {
            phases.append(phaseJson(intro.displayPrice, intro.price, currencyCode, period: intro.period,
                cycles: intro.periodCount, recurrence: recurrenceFinite))
        }
        phases.append(phaseJson(product.displayPrice, product.price, currencyCode, period: subscription.subscriptionPeriod,
            cycles: 0, recurrence: recurrenceInfinite))
    } else {
        phases.append(phaseJson(product.displayPrice, product.price, currencyCode, period: nil, cycles: 0, recurrence: recurrenceNone))
    }

    return [
        "productId": product.id,
        "type": productType(product.type),
        "title": product.displayName,
        "name": product.displayName,
        "description": product.description,
        "price": product.displayPrice,
        "priceMicros": micros(product.price),
        "currencyCode": currencyCode,
        // one plan, bought without an offer token
        "offers": [["offerToken": "", "offerId": "", "basePlanId": "", "tags": [String](), "pricingPhases": phases]],
    ]
}

private func phaseJson(_ displayPrice: String, _ price: Decimal, _ currencyCode: String, period: Product.SubscriptionPeriod?,
        cycles: Int, recurrence: Int) -> [String: Any] {
    return [
        "price": displayPrice,
        "priceMicros": micros(price),
        "currencyCode": currencyCode,
        "billingPeriod": period.map(isoPeriod) ?? "",
        "billingCycleCount": cycles,
        "recurrenceMode": recurrence,
    ]
}

private func purchaseJson(_ transaction: Transaction, jws: String, state: Int, acknowledged: Bool,
        autoRenewing: Bool = false, suspended: Bool = false) -> [String: Any] {
    return [
        "orderId": String(transaction.id),
        "productIds": [transaction.productID],
        "productType": productType(transaction.productType),
        // kept by renewals, like a Google Play token
        "purchaseToken": String(transaction.originalID),
        "purchaseTime": milliseconds(transaction.purchaseDate),
        "state": state,
        "quantity": transaction.purchasedQuantity,
        "acknowledged": acknowledged,
        "autoRenewing": autoRenewing,
        "suspended": suspended,
        "packageName": transaction.appBundleID,
        "obfuscatedAccountId": transaction.appAccountToken?.uuidString.lowercased() ?? "",
        "originalJson": String(decoding: transaction.jsonRepresentation, as: UTF8.self),
        // the JWS to verify on a server
        "signature": jws,
    ]
}

// No transaction yet, so no token
private func pendingJson(_ product: Product) -> [String: Any] {
    return [
        "productIds": [product.id],
        "productType": productType(product.type),
        "purchaseTime": milliseconds(Date()),
        "state": statePending,
        "quantity": 1,
        "packageName": Bundle.main.bundleIdentifier ?? "",
    ]
}

private func willAutoRenew(_ status: Product.SubscriptionInfo.Status) -> Bool {
    if case .verified(let renewalInfo) = status.renewalInfo {
        return renewalInfo.willAutoRenew
    }
    return true
}

// ISO 8601, like P1M
private func isoPeriod(_ period: Product.SubscriptionPeriod) -> String {
    switch period.unit {
    case .day: return "P\(period.value)D"
    case .week: return "P\(period.value)W"
    case .month: return "P\(period.value)M"
    case .year: return "P\(period.value)Y"
    @unknown default: return ""
    }
}

private func response(for error: Error) -> Response {
    switch error {
    case StoreKitError.userCancelled:
        return .userCanceled
    case StoreKitError.networkError(_):
        return .networkError
    case StoreKitError.notAvailableInStorefront, Product.PurchaseError.productUnavailable, Product.PurchaseError.ineligibleForOffer:
        return .itemUnavailable
    case Product.PurchaseError.purchaseNotAllowed:
        return .billingUnavailable
    case StoreKitError.notEntitled, is Product.PurchaseError:
        return .developerError
    default:
        return .fatalError
    }
}

private func answerFinish(_ purchaseToken: String, consume: Bool, _ response: Response, _ message: String) {
    if consume {
        QuantumStoreKitPurchaseConsumed(purchaseToken, response.rawValue, message)
    } else {
        QuantumStoreKitPurchaseAcknowledged(purchaseToken, response.rawValue, message)
    }
}

private func micros(_ price: Decimal) -> Int64 {
    // rounded before the integer conversion
    var value = price * 1_000_000
    var rounded = Decimal()
    NSDecimalRound(&rounded, &value, 0, .plain)
    return NSDecimalNumber(decimal: rounded).int64Value
}

private func milliseconds(_ date: Date) -> Int64 {
    return Int64((date.timeIntervalSince1970 * 1000).rounded())
}

private func json(_ value: Any) -> String {
    guard JSONSerialization.isValidJSONObject(value), let data = try? JSONSerialization.data(withJSONObject: value) else {
        return "[]"
    }
    return String(decoding: data, as: UTF8.self)
}

private func warn(_ message: String) {
    NSLog("Quantum: %@", message)
}
