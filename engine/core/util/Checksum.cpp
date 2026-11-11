// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Checksum.h"

#include <cstdio>
#include <fstream>
#include <vector>

using namespace quantum;

namespace{

    // K[i] is the low 32 bits of the fractional part of the cube root of the i-th prime
    // (FIPS 180-4 section 4.2.2), reproduced here from that definition rather than transcribed.
    const unsigned int SHA256_K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    inline unsigned int rotr(unsigned int value, unsigned int bits){
        return (value >> bits) | (value << (32 - bits));
    }

    // The running state of one SHA-256, fed whole 64-byte blocks.
    struct Sha256State{
        unsigned int h[8] = {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
        };
        // Message bytes only: the padding and the length field must not feed back into it.
        unsigned long long messageBytes = 0;
        std::vector<unsigned char> leftover;

        void transform(const unsigned char* block);
        // Everything that enters the stream, counted or not.
        void append(const unsigned char* data, size_t size);
        void feed(const unsigned char* data, size_t size);
        void finish(unsigned char outDigest[32]);
    };

    void Sha256State::transform(const unsigned char* block){
        unsigned int w[64];
        for (int i = 0; i < 16; i++){
            w[i] = (static_cast<unsigned int>(block[i * 4]) << 24) |
                   (static_cast<unsigned int>(block[i * 4 + 1]) << 16) |
                   (static_cast<unsigned int>(block[i * 4 + 2]) << 8) |
                   (static_cast<unsigned int>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 64; i++){
            const unsigned int s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            const unsigned int s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        unsigned int a = h[0], b = h[1], c = h[2], d = h[3];
        unsigned int e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; i++){
            const unsigned int S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const unsigned int ch = (e & f) ^ ((~e) & g);
            const unsigned int temp1 = hh + S1 + ch + SHA256_K[i] + w[i];
            const unsigned int S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
            const unsigned int temp2 = S0 + maj;

            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void Sha256State::append(const unsigned char* data, size_t size){
        if (!leftover.empty()){
            const size_t need = 64 - leftover.size();
            const size_t take = size < need ? size : need;
            leftover.insert(leftover.end(), data, data + take);
            data += take;
            size -= take;
            if (leftover.size() == 64){
                transform(leftover.data());
                leftover.clear();
            } else {
                return;
            }
        }

        while (size >= 64){
            transform(data);
            data += 64;
            size -= 64;
        }
        if (size > 0) leftover.insert(leftover.end(), data, data + size);
    }

    void Sha256State::feed(const unsigned char* data, size_t size){
        messageBytes += size;
        append(data, size);
    }

    void Sha256State::finish(unsigned char outDigest[32]){
        const unsigned long long messageBits = messageBytes * 8ull;

        // 0x80, then enough zeros that the stream sits one block minus the length field from a
        // boundary, then the 64-bit big-endian bit count of the message.
        const size_t rest = static_cast<size_t>(messageBytes % 64ull);
        const size_t zeros = (55 - rest + 64) % 64;
        const unsigned char marker = 0x80;
        append(&marker, 1);
        for (size_t i = 0; i < zeros; i++){
            const unsigned char zero = 0x00;
            append(&zero, 1);
        }

        unsigned char lengthBytes[8];
        for (int i = 0; i < 8; i++){
            lengthBytes[i] = static_cast<unsigned char>(messageBits >> (56 - i * 8));
        }
        append(lengthBytes, 8);

        for (int i = 0; i < 8; i++){
            outDigest[i * 4]     = static_cast<unsigned char>(h[i] >> 24);
            outDigest[i * 4 + 1] = static_cast<unsigned char>(h[i] >> 16);
            outDigest[i * 4 + 2] = static_cast<unsigned char>(h[i] >> 8);
            outDigest[i * 4 + 3] = static_cast<unsigned char>(h[i]);
        }
    }

    std::string toHex(const unsigned char* digest, size_t size){
        static const char* DIGITS = "0123456789abcdef";
        std::string out;
        out.reserve(size * 2);
        for (size_t i = 0; i < size; i++){
            out.push_back(DIGITS[digest[i] >> 4]);
            out.push_back(DIGITS[digest[i] & 0x0F]);
        }
        return out;
    }

    unsigned int CRC32_TABLE[256];
    bool crc32TableReady = false;

    void buildCrc32Table(){
        for (unsigned int i = 0; i < 256; i++){
            unsigned int c = i;
            for (int k = 0; k < 8; k++){
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            CRC32_TABLE[i] = c;
        }
        crc32TableReady = true;
    }

}

std::string quantum::sha256Hex(const unsigned char* data, size_t size){
    Sha256State state;
    state.feed(data, size);
    unsigned char digest[32];
    state.finish(digest);
    return toHex(digest, 32);
}

std::string quantum::sha256Hex(const std::string& text){
    return sha256Hex(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

std::string quantum::sha256HexOfFile(const std::string& path, std::string& errorOut){
    errorOut.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()){
        errorOut = "cannot open " + path;
        return std::string();
    }

    Sha256State state;
    std::vector<char> chunk(65536);
    while (stream){
        stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = stream.gcount();
        if (got > 0) state.feed(reinterpret_cast<const unsigned char*>(chunk.data()), static_cast<size_t>(got));
    }
    if (stream.bad()){
        errorOut = "cannot read " + path;
        return std::string();
    }

    unsigned char digest[32];
    state.finish(digest);
    return toHex(digest, 32);
}

unsigned int quantum::crc32Of(const unsigned char* data, size_t size){
    if (!crc32TableReady) buildCrc32Table();
    unsigned int c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++){
        c = CRC32_TABLE[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}
