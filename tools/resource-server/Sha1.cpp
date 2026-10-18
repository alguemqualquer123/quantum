// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "Sha1.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace doriax::rs {

namespace {
    inline uint32_t rol(uint32_t value, uint32_t bits) {
        return (value << bits) | (value >> (32 - bits));
    }
}

Sha1::Sha1()
    : h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u},
      totalBytes_(0), bufLen_(0) {
}

void Sha1::transform(const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4 + 0]) << 24) |
               (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) |
               (uint32_t(block[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);   k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;              k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;              k = 0xCA62C1D6u; }
        uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = temp;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
}

void Sha1::update(const void* data, size_t length) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    totalBytes_ += length;
    while (length > 0) {
        size_t take = 64 - bufLen_;
        if (take > length) take = length;
        memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        length -= take;
        if (bufLen_ == 64) {
            transform(buf_);
            bufLen_ = 0;
        }
    }
}

std::string Sha1::hexDigest() {
    // Finalizes the stream: the object must not be updated afterwards.
    uint64_t totalBits = totalBytes_ * 8ull;

    uint8_t pad[72];
    size_t n = bufLen_;
    memcpy(pad, buf_, n);
    pad[n++] = 0x80;
    if (n > 56) {
        while (n < 64) pad[n++] = 0x00;
        transform(pad);
        n = 0;
    }
    while (n < 56) pad[n++] = 0x00;
    for (int i = 7; i >= 0; --i) pad[n++] = uint8_t((totalBits >> (i * 8)) & 0xFF);
    transform(pad);

    std::string out;
    out.reserve(40);
    static const char* hex = "0123456789abcdef";
    for (int w = 0; w < 5; ++w) {
        uint32_t v = h_[w];
        for (int b = 3; b >= 0; --b) {
            out.push_back(hex[(v >> (b * 8 + 4)) & 0xF]);
            out.push_back(hex[(v >> (b * 8)) & 0xF]);
        }
    }
    return out;
}

bool hashFile(const std::string& pathUtf8, std::string& hexOut, std::string& errorOut) {
#ifdef _WIN32
    FILE* f = nullptr;
    if (fopen_s(&f, pathUtf8.c_str(), "rb") != 0 || !f) {
        errorOut = "cannot open file: " + pathUtf8;
        return false;
    }
#else
    FILE* f = fopen(pathUtf8.c_str(), "rb");
    if (!f) {
        errorOut = "cannot open file: " + pathUtf8;
        return false;
    }
#endif
    Sha1 sha;
    char chunk[65536];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        sha.update(chunk, got);
    }
    bool hadError = ferror(f) != 0;
    fclose(f);
    if (hadError) {
        errorOut = "read error: " + pathUtf8;
        return false;
    }
    hexOut = sha.hexDigest();
    return true;
}

}
