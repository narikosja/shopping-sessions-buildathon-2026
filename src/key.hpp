#pragma once

#include "input.hpp"
#include <bit>
#include <deque>

class Key {
public:
    Key() = default;

    bool empty() const { return bits_ == 0; }

    Text text(std::array<char, 9>& buffer) const {
        if (!compact()) return empty() ? Text{} : referenced();
        uint64_t payload = bits_ >> 1;
        size_t length = 0;
        do {
            buffer[length++] = static_cast<char>(payload & 127);
        } while ((payload >>= 7) != 0);
        return {buffer.data(), length};
    }

    uint64_t hash() const {
        if (!compact()) return std::hash<Text>{}(empty() ? Text{} : referenced());
        uint64_t x = bits_;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    bool operator==(Key other) const {
        return bits_ == other.bits_ ||
               (!compact() && !other.compact() && !empty() && !other.empty() &&
                referenced() == other.referenced());
    }

    bool operator<(Key other) const {
        std::array<char, 9> left, right;
        return text(left) < other.text(right);
    }

private:
    explicit Key(uint64_t bits) : bits_(bits) {}

    bool compact() const { return bits_ & 1; }

    Text referenced() const {
        return *reinterpret_cast<const Text*>(static_cast<uintptr_t>(bits_));
    }

    uint64_t bits_ = 0;
    friend class KeyArena;
};

class KeyArena {
public:
    Key make(Text text) {
        if (text.empty()) return {};
        if (text.size() == 8 || text.size() == 9) {
            uint64_t first;
            std::memcpy(&first, text.data(), sizeof(first));
            if constexpr (std::endian::native == std::endian::big) first = __builtin_bswap64(first);
            const unsigned last = text.size() == 9 ? static_cast<unsigned char>(text[8]) : 1;
            constexpr uint64_t high_bits = 0x8080808080808080ULL;
            const uint64_t zeros = (first - 0x0101010101010101ULL) & ~first & high_bits;
            if (!((first & high_bits) | zeros) && last && last < 128) {
                first = (first & 0x007f007f007f007fULL) | ((first & 0x7f007f007f007f00ULL) >> 1);
                first = (first & 0x00003fff00003fffULL) | ((first & 0x3fff00003fff0000ULL) >> 2);
                first = (first & 0x000000000fffffffULL) | ((first & 0x0fffffff00000000ULL) >> 4);
                const uint64_t ninth = text.size() == 9 ? uint64_t{last} << 56 : 0;
                return Key(((first | ninth) << 1) | 1);
            }
        } else if (text.size() < 8) {
            uint64_t payload = 0;
            unsigned shift = 0;
            for (unsigned char byte : text) {
                if (!byte || byte >= 128) return store(text);
                payload |= uint64_t{byte} << shift;
                shift += 7;
            }
            return Key((payload << 1) | 1);
        }
        return store(text);
    }

private:
    Key store(Text text) {
        static_assert(alignof(Text) >= 2);
        references_.push_back(text);
        return Key(reinterpret_cast<uintptr_t>(&references_.back()));
    }

    std::deque<Text> references_;
};
