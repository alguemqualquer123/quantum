// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AiTypes.h"

#include <memory>

namespace quantum::editor::ai {

class Provider {
public:
    virtual ~Provider() = default;

    virtual ProviderId id() const = 0;
    virtual HttpRequest buildRequest(const ProviderRequest& request) const = 0;
    virtual ProviderResponse parseResponse(const std::string& body) const = 0;

    // Streaming support: build a request with stream=true and parse incremental chunks.
    // Only providers that override supportsStreaming() implement them; the base has no
    // fallback payload format, so it must stay pure-virtual rather than link-erroring.
    virtual HttpRequest buildStreamingRequest(const ProviderRequest& request) const = 0;
    virtual ProviderResponse parseStreamingChunk(const std::string& line,
                                                 ProviderResponse& accumulated) const = 0;
    virtual bool supportsStreaming() const { return false; }
};

std::unique_ptr<Provider> createProvider(ProviderId provider);

} // namespace quantum::editor::ai
