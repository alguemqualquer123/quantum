// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include "AiTypes.h"

#include <memory>

namespace doriax::editor::ai {

class Provider {
public:
    virtual ~Provider() = default;

    virtual ProviderId id() const = 0;
    virtual HttpRequest buildRequest(const ProviderRequest& request) const = 0;
    virtual ProviderResponse parseResponse(const std::string& body) const = 0;

    // Streaming support: build a request with stream=true and parse incremental chunks.
    virtual HttpRequest buildStreamingRequest(const ProviderRequest& request) const;
    virtual ProviderResponse parseStreamingChunk(const std::string& line,
                                                 ProviderResponse& accumulated) const;
    virtual bool supportsStreaming() const { return false; }
};

std::unique_ptr<Provider> createProvider(ProviderId provider);

} // namespace doriax::editor::ai
