#pragma once

#include "input.hpp"

#include <mutex>
#include <unordered_set>

namespace shopping {

using Id = uint64_t;
constexpr Id external_id = Id{1} << 63;

// Short non-NUL ASCII strings fit in nine 7-bit fields. Zero padding gives
// their codes the same ordering as the original bytes.
class Identifiers {
public:
    Id get(Text text) {
        if (text.size() == 9) {
            uint64_t prefix;
            std::memcpy(&prefix, text.data(), 8);
            constexpr uint64_t high_bits = 0x8080808080808080ULL;
            constexpr uint64_t low_bits = 0x0101010101010101ULL;
            const auto last = static_cast<unsigned char>(text[8]);
            if (((prefix | (prefix - low_bits)) & high_bits) == 0 && last > 0 && last < 128) {
                return pack_nine(prefix, last);
            }
        } else if (text.size() < 9) {
            Id result = 0;
            for (unsigned char byte : text) {
                if (byte == 0 || byte >= 128) return store(text);
                result = (result << 7) | byte;
            }
            return result << ((9 - text.size()) * 7);
        }
        return store(text);
    }

    static Text decode(Id id, std::array<char, 9>& buffer) {
        if (id & external_id) {
            return *reinterpret_cast<const Text*>(id & ~external_id);
        }
        size_t length = 0;
        while (id) {
            buffer[length++] = static_cast<char>(id >> 56);
            id = (id << 7) & ~external_id;
        }
        return {buffer.data(), length};
    }

    static bool byte_less(Id a, Id b) {
        if (((a | b) & external_id) == 0) return a < b;
        std::array<char, 9> first, second;
        return decode(a, first) < decode(b, second);
    }

private:
    static Id pack_nine(uint64_t prefix, unsigned char last) {
        prefix = __builtin_bswap64(prefix);
        // Compress away the high bit of every ASCII byte.
        Id result = prefix & 0x000000000000007fULL;
        result |= (prefix & 0x0000000000007f00ULL) >> 1;
        result |= (prefix & 0x00000000007f0000ULL) >> 2;
        result |= (prefix & 0x000000007f000000ULL) >> 3;
        result |= (prefix & 0x0000007f00000000ULL) >> 4;
        result |= (prefix & 0x00007f0000000000ULL) >> 5;
        result |= (prefix & 0x007f000000000000ULL) >> 6;
        result |= (prefix & 0x7f00000000000000ULL) >> 7;
        return (result << 7) | last;
    }

    Id store(Text text) {
        auto& shard = shards_[TextHash{}(text) % shards_.size()];
        std::lock_guard lock(shard.mutex);
        const auto it = shard.values.insert(text).first;
        return reinterpret_cast<uintptr_t>(&*it) | external_id;
    }

    struct Shard {
        std::mutex mutex;
        std::unordered_set<Text, TextHash> values;
    };
    std::array<Shard, 64> shards_;
};

}  // namespace shopping
