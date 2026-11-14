// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdint.h>
#include <cstddef>

namespace quantum::editor{
    class SHA1{

    public:
        inline static uint32_t rol(const uint32_t value, const size_t bits) {
            return (value << bits) | (value >> (32 - bits));
        }

        class Context {
        public:
            Context() : h0(0x67452301), h1(0xEFCDAB89), h2(0x98BADCFE),
                        h3(0x10325476), h4(0xC3D2E1F0), totalBits(0), buffered(0) {}

            void update(const void* data, size_t size) {
                const auto* bytes = static_cast<const uint8_t*>(data);
                totalBits += static_cast<uint64_t>(size) * 8;
                while (size > 0) {
                    const size_t copied = (size < sizeof(buffer) - buffered) ? size : sizeof(buffer) - buffered;
                    std::memcpy(buffer + buffered, bytes, copied);
                    buffered += copied;
                    bytes += copied;
                    size -= copied;
                    if (buffered == sizeof(buffer)) {
                        process(buffer);
                        buffered = 0;
                    }
                }
            }

            void update(const std::string& input) {
                update(input.data(), input.size());
            }

            std::string final() {
                const uint64_t originalBits = totalBits;
                const uint8_t one = 0x80;
                update(&one, 1);
                const uint8_t zero = 0;
                while (buffered != 56) update(&zero, 1);

                uint8_t length[8];
                for (int i = 7; i >= 0; --i) {
                    length[7 - i] = static_cast<uint8_t>((originalBits >> (i * 8)) & 0xFF);
                }
                update(length, sizeof(length));

                std::ostringstream result;
                result << std::hex << std::setfill('0');
                result << std::setw(8) << h0;
                result << std::setw(8) << h1;
                result << std::setw(8) << h2;
                result << std::setw(8) << h3;
                result << std::setw(8) << h4;
                return result.str();
            }

        private:
            uint32_t h0;
            uint32_t h1;
            uint32_t h2;
            uint32_t h3;
            uint32_t h4;
            uint64_t totalBits;
            uint8_t buffer[64] = {};
            size_t buffered;

            void process(const uint8_t* bytes) {
                uint32_t w[80];
                for (int i = 0; i < 16; ++i) {
                    w[i] = (static_cast<uint32_t>(bytes[i * 4 + 0]) << 24) |
                        (static_cast<uint32_t>(bytes[i * 4 + 1]) << 16) |
                        (static_cast<uint32_t>(bytes[i * 4 + 2]) << 8) |
                        static_cast<uint32_t>(bytes[i * 4 + 3]);
                }
                for (int i = 16; i < 80; ++i) {
                    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                }

                uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
                for (int i = 0; i < 80; ++i) {
                    uint32_t f, k;
                    if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
                    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                    const uint32_t temp = rol(a, 5) + f + e + k + w[i];
                    e = d;
                    d = c;
                    c = rol(b, 30);
                    b = a;
                    a = temp;
                }
                h0 += a;
                h1 += b;
                h2 += c;
                h3 += d;
                h4 += e;
            }
        };

        inline static std::string hash(const std::string& input) {
            Context context;
            context.update(input);
            return context.final();
        }
    };
}
