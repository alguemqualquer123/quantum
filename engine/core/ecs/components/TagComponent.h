// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include "../Component.h"
#include <cstdint>
#include <string>

namespace doriax {

// Lightweight tag component for fast entity filtering and categorization.
// Uses a 64-bit bitmask for O(1) category checks, plus an optional string
// tag for more specific identification.
//
// Usage:
//   TagComponent tag;
//   tag.setCategory(TagCategory::ENEMY | TagCategory::DAMAGABLE);
//   tag.setTag("goblin");
//
//   // Fast bitmask check:
//   if (tag.hasCategory(TagCategory::ENEMY)) { ... }
//
//   // Combined check:
//   if (tag.hasAllCategories(TagCategory::ENEMY | TagCategory::DAMAGABLE)) { ... }
//
struct TagComponent : public Component {
    // Predefined category bits for common use cases.
    // Users can define custom bits in the upper range (bits 32-63).
    enum Category : uint64_t {
        NONE            = 0,
        PLAYER          = 1ULL << 0,
        ENEMY           = 1ULL << 1,
        NPC             = 1ULL << 2,
        PROJECTILE      = 1ULL << 3,
        PICKUP          = 1ULL << 4,
        DAMAGABLE       = 1ULL << 5,
        TRIGGER         = 1ULL << 6,
        STATIC          = 1ULL << 7,
        KINEMATIC       = 1ULL << 8,
        UI_ELEMENT      = 1ULL << 9,
        ENVIRONMENT     = 1ULL << 10,
        LIGHT_SOURCE    = 1ULL << 11,
        AUDIO_SOURCE    = 1ULL << 12,
        CAMERA          = 1ULL << 13,
        SPAWNER         = 1ULL << 14,
        COLLECTIBLE     = 1ULL << 15,
        OBSTACLE        = 1ULL << 16,
        DEBRIS          = 1ULL << 17,
        EFFECT          = 1ULL << 18,
        DEBUG           = 1ULL << 19,
        // Bits 20-31 reserved for engine use
        // Bits 32-63 available for user-defined categories
        USER_0          = 1ULL << 32,
        USER_1          = 1ULL << 33,
        USER_2          = 1ULL << 34,
        USER_3          = 1ULL << 35,
        USER_4          = 1ULL << 36,
        USER_5          = 1ULL << 37,
        USER_6          = 1ULL << 38,
        USER_7          = 1ULL << 39,
    };

    // Bitmask of categories this entity belongs to.
    uint64_t categories = NONE;

    // Optional string tag for more specific identification (e.g., "goblin_archer").
    std::string tag;

    // Layer index (0-31) for rendering/physics layer filtering.
    uint8_t layer = 0;

    // ---- Category operations ----

    void setCategory(uint64_t categories) {
        this->categories = categories;
    }

    void addCategory(uint64_t category) {
        categories |= category;
    }

    void removeCategory(uint64_t category) {
        categories &= ~category;
    }

    bool hasCategory(uint64_t category) const {
        return (categories & category) == category;
    }

    bool hasAnyCategory(uint64_t mask) const {
        return (categories & mask) != 0;
    }

    bool hasAllCategories(uint64_t mask) const {
        return (categories & mask) == mask;
    }

    bool hasNoCategory() const {
        return categories == NONE;
    }

    // ---- Tag operations ----

    void setTag(const std::string& newTag) {
        tag = newTag;
    }

    const std::string& getTag() const {
        return tag;
    }

    bool hasTag() const {
        return !tag.empty();
    }

    bool isTag(const std::string& compareTag) const {
        return tag == compareTag;
    }

    // ---- Layer operations ----

    void setLayer(uint8_t newLayer) {
        layer = (newLayer < 32) ? newLayer : 0;
    }

    uint8_t getLayer() const {
        return layer;
    }

    bool isOnLayer(uint8_t otherLayer) const {
        return layer == otherLayer;
    }

    bool isOnLayers(uint32_t layerMask) const {
        return (layerMask & (1U << layer)) != 0;
    }
};

}
