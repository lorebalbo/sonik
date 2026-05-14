#pragma once

// Public-domain SHA-1 implementation by Steve Reid <steve@edmweb.com>,
// originally published as "100% Public Domain". Adapted into a single
// header here for in-process device-ID hashing only — NOT for any
// security-sensitive use. Returns the LOW 64 bits of the 160-bit digest
// (i.e. the last 8 bytes of the digest), as mandated by PRD-0040.

#include <cstdint>
#include <cstring>
#include <juce_core/juce_core.h>

namespace sonik::midi::sha1
{
    namespace detail
    {
        struct Ctx
        {
            uint32_t state[5];
            uint32_t count[2];
            uint8_t  buffer[64];
        };

        inline uint32_t rol (uint32_t value, int bits) noexcept
        {
            return (value << bits) | (value >> (32 - bits));
        }

        inline uint32_t blk0 (uint32_t* block, int i) noexcept
        {
        #if JUCE_LITTLE_ENDIAN
            block[i] = (rol (block[i], 24) & 0xFF00FF00u)
                     | (rol (block[i],  8) & 0x00FF00FFu);
        #endif
            return block[i];
        }

        inline uint32_t blk (uint32_t* block, int i) noexcept
        {
            block[i & 15] = rol (block[(i + 13) & 15] ^ block[(i + 8) & 15]
                                ^ block[(i + 2) & 15] ^ block[i & 15], 1);
            return block[i & 15];
        }

        #define SONIK_SHA1_R0(v,w,x,y,z,i) z += ((w & (x ^ y)) ^ y) + blk0(block, i) + 0x5A827999u + rol(v, 5); w = rol(w, 30);
        #define SONIK_SHA1_R1(v,w,x,y,z,i) z += ((w & (x ^ y)) ^ y) + blk(block, i)  + 0x5A827999u + rol(v, 5); w = rol(w, 30);
        #define SONIK_SHA1_R2(v,w,x,y,z,i) z += (w ^ x ^ y)         + blk(block, i)  + 0x6ED9EBA1u + rol(v, 5); w = rol(w, 30);
        #define SONIK_SHA1_R3(v,w,x,y,z,i) z += (((w | x) & y) | (w & x)) + blk(block, i) + 0x8F1BBCDCu + rol(v, 5); w = rol(w, 30);
        #define SONIK_SHA1_R4(v,w,x,y,z,i) z += (w ^ x ^ y)         + blk(block, i)  + 0xCA62C1D6u + rol(v, 5); w = rol(w, 30);

        inline void transform (uint32_t state[5], const uint8_t buffer[64]) noexcept
        {
            uint32_t block[16];
            std::memcpy (block, buffer, 64);

            uint32_t a = state[0];
            uint32_t b = state[1];
            uint32_t c = state[2];
            uint32_t d = state[3];
            uint32_t e = state[4];

            SONIK_SHA1_R0(a,b,c,d,e, 0); SONIK_SHA1_R0(e,a,b,c,d, 1); SONIK_SHA1_R0(d,e,a,b,c, 2); SONIK_SHA1_R0(c,d,e,a,b, 3);
            SONIK_SHA1_R0(b,c,d,e,a, 4); SONIK_SHA1_R0(a,b,c,d,e, 5); SONIK_SHA1_R0(e,a,b,c,d, 6); SONIK_SHA1_R0(d,e,a,b,c, 7);
            SONIK_SHA1_R0(c,d,e,a,b, 8); SONIK_SHA1_R0(b,c,d,e,a, 9); SONIK_SHA1_R0(a,b,c,d,e,10); SONIK_SHA1_R0(e,a,b,c,d,11);
            SONIK_SHA1_R0(d,e,a,b,c,12); SONIK_SHA1_R0(c,d,e,a,b,13); SONIK_SHA1_R0(b,c,d,e,a,14); SONIK_SHA1_R0(a,b,c,d,e,15);
            SONIK_SHA1_R1(e,a,b,c,d,16); SONIK_SHA1_R1(d,e,a,b,c,17); SONIK_SHA1_R1(c,d,e,a,b,18); SONIK_SHA1_R1(b,c,d,e,a,19);
            SONIK_SHA1_R2(a,b,c,d,e,20); SONIK_SHA1_R2(e,a,b,c,d,21); SONIK_SHA1_R2(d,e,a,b,c,22); SONIK_SHA1_R2(c,d,e,a,b,23);
            SONIK_SHA1_R2(b,c,d,e,a,24); SONIK_SHA1_R2(a,b,c,d,e,25); SONIK_SHA1_R2(e,a,b,c,d,26); SONIK_SHA1_R2(d,e,a,b,c,27);
            SONIK_SHA1_R2(c,d,e,a,b,28); SONIK_SHA1_R2(b,c,d,e,a,29); SONIK_SHA1_R2(a,b,c,d,e,30); SONIK_SHA1_R2(e,a,b,c,d,31);
            SONIK_SHA1_R2(d,e,a,b,c,32); SONIK_SHA1_R2(c,d,e,a,b,33); SONIK_SHA1_R2(b,c,d,e,a,34); SONIK_SHA1_R2(a,b,c,d,e,35);
            SONIK_SHA1_R2(e,a,b,c,d,36); SONIK_SHA1_R2(d,e,a,b,c,37); SONIK_SHA1_R2(c,d,e,a,b,38); SONIK_SHA1_R2(b,c,d,e,a,39);
            SONIK_SHA1_R3(a,b,c,d,e,40); SONIK_SHA1_R3(e,a,b,c,d,41); SONIK_SHA1_R3(d,e,a,b,c,42); SONIK_SHA1_R3(c,d,e,a,b,43);
            SONIK_SHA1_R3(b,c,d,e,a,44); SONIK_SHA1_R3(a,b,c,d,e,45); SONIK_SHA1_R3(e,a,b,c,d,46); SONIK_SHA1_R3(d,e,a,b,c,47);
            SONIK_SHA1_R3(c,d,e,a,b,48); SONIK_SHA1_R3(b,c,d,e,a,49); SONIK_SHA1_R3(a,b,c,d,e,50); SONIK_SHA1_R3(e,a,b,c,d,51);
            SONIK_SHA1_R3(d,e,a,b,c,52); SONIK_SHA1_R3(c,d,e,a,b,53); SONIK_SHA1_R3(b,c,d,e,a,54); SONIK_SHA1_R3(a,b,c,d,e,55);
            SONIK_SHA1_R3(e,a,b,c,d,56); SONIK_SHA1_R3(d,e,a,b,c,57); SONIK_SHA1_R3(c,d,e,a,b,58); SONIK_SHA1_R3(b,c,d,e,a,59);
            SONIK_SHA1_R4(a,b,c,d,e,60); SONIK_SHA1_R4(e,a,b,c,d,61); SONIK_SHA1_R4(d,e,a,b,c,62); SONIK_SHA1_R4(c,d,e,a,b,63);
            SONIK_SHA1_R4(b,c,d,e,a,64); SONIK_SHA1_R4(a,b,c,d,e,65); SONIK_SHA1_R4(e,a,b,c,d,66); SONIK_SHA1_R4(d,e,a,b,c,67);
            SONIK_SHA1_R4(c,d,e,a,b,68); SONIK_SHA1_R4(b,c,d,e,a,69); SONIK_SHA1_R4(a,b,c,d,e,70); SONIK_SHA1_R4(e,a,b,c,d,71);
            SONIK_SHA1_R4(d,e,a,b,c,72); SONIK_SHA1_R4(c,d,e,a,b,73); SONIK_SHA1_R4(b,c,d,e,a,74); SONIK_SHA1_R4(a,b,c,d,e,75);
            SONIK_SHA1_R4(e,a,b,c,d,76); SONIK_SHA1_R4(d,e,a,b,c,77); SONIK_SHA1_R4(c,d,e,a,b,78); SONIK_SHA1_R4(b,c,d,e,a,79);

            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
        }

        #undef SONIK_SHA1_R0
        #undef SONIK_SHA1_R1
        #undef SONIK_SHA1_R2
        #undef SONIK_SHA1_R3
        #undef SONIK_SHA1_R4

        inline void init (Ctx& ctx) noexcept
        {
            ctx.state[0] = 0x67452301u;
            ctx.state[1] = 0xEFCDAB89u;
            ctx.state[2] = 0x98BADCFEu;
            ctx.state[3] = 0x10325476u;
            ctx.state[4] = 0xC3D2E1F0u;
            ctx.count[0] = ctx.count[1] = 0;
        }

        inline void update (Ctx& ctx, const uint8_t* data, size_t len) noexcept
        {
            uint32_t j = (ctx.count[0] >> 3) & 63u;
            if ((ctx.count[0] += static_cast<uint32_t> (len << 3)) < (len << 3))
                ctx.count[1]++;
            ctx.count[1] += static_cast<uint32_t> (len >> 29);

            size_t i = 0;
            if ((j + len) > 63)
            {
                size_t partial = 64 - j;
                std::memcpy (&ctx.buffer[j], data, partial);
                transform (ctx.state, ctx.buffer);
                for (i = partial; i + 63 < len; i += 64)
                    transform (ctx.state, &data[i]);
                j = 0;
            }
            std::memcpy (&ctx.buffer[j], &data[i], len - i);
        }

        inline void finalise (Ctx& ctx, uint8_t digest[20]) noexcept
        {
            uint8_t finalcount[8];
            for (int i = 0; i < 8; ++i)
                finalcount[i] = static_cast<uint8_t> (
                    (ctx.count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 0xFFu);

            uint8_t c = 0x80u;
            update (ctx, &c, 1);
            while ((ctx.count[0] & 504u) != 448u)
            {
                c = 0x00u;
                update (ctx, &c, 1);
            }
            update (ctx, finalcount, 8);

            for (int i = 0; i < 20; ++i)
                digest[i] = static_cast<uint8_t> (
                    (ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFFu);
        }
    } // namespace detail

    /** Returns the low 64 bits of the SHA-1 digest of the given UTF-8 string.
        Used purely as a stable, in-process device-ID derivation per PRD-0040. */
    inline uint64_t sha1Low64 (const juce::String& input) noexcept
    {
        const auto utf8 = input.toRawUTF8();
        const auto byteLen = std::strlen (utf8);

        detail::Ctx ctx{};
        detail::init (ctx);
        detail::update (ctx, reinterpret_cast<const uint8_t*> (utf8), byteLen);

        uint8_t digest[20];
        detail::finalise (ctx, digest);

        // "Low 64 bits" = last 8 bytes of the 160-bit digest, big-endian.
        uint64_t result = 0;
        for (int i = 12; i < 20; ++i)
            result = (result << 8) | static_cast<uint64_t> (digest[i]);
        return result;
    }
} // namespace sonik::midi::sha1
