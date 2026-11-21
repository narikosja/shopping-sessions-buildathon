#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include <immintrin.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shopping {

namespace fs = std::filesystem;
using Text = std::string_view;

constexpr int64_t day_ms = 86'400'000;
constexpr int64_t lookback_ms = 7 * day_ms;
constexpr int64_t gap_ms = 1'800'000;
constexpr int64_t invalid_ts = std::numeric_limits<int64_t>::min();

inline uint64_t mix(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

struct TextHash {
    size_t operator()(Text text) const {
        // All bytes contribute to the hash. Callers still compare the original strings.
        uint64_t value = text.size();
        while (text.size() >= 8) {
            uint64_t word;
            std::memcpy(&word, text.data(), sizeof(word));
            value = (value ^ word) * 0x9e3779b185ebca87ULL;
            text.remove_prefix(8);
        }
        uint64_t tail = 0;
        if (!text.empty()) std::memcpy(&tail, text.data(), text.size());
        return mix(value ^ tail);
    }
};

class MappedFile {
public:
    explicit MappedFile(const fs::path& path) {
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd == -1) throw std::system_error(errno, std::generic_category(), path.string());
        struct stat info {};
        if (fstat(fd, &info) == -1) {
            int error = errno;
            close(fd);
            throw std::system_error(error, std::generic_category(), path.string());
        }
        if (info.st_size > 0) {
            void* memory = mmap(nullptr, info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            int error = errno;
            close(fd);
            if (memory == MAP_FAILED) {
                throw std::system_error(error, std::generic_category(), path.string());
            }
            bytes_ = {static_cast<const char*>(memory), static_cast<size_t>(info.st_size)};
        } else {
            close(fd);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() {
        if (!bytes_.empty()) munmap(const_cast<char*>(bytes_.data()), bytes_.size());
    }

    Text rows() const {
        const size_t header = bytes_.find('\n');
        return header == Text::npos ? Text{} : bytes_.substr(header + 1);
    }

private:
    Text bytes_;
};

class Inputs {
public:
    Text load(const fs::path& path) {
        files_.push_back(std::make_unique<MappedFile>(path));
        return files_.back()->rows();
    }

    std::vector<Text> chunks(const fs::path& folder) {
        constexpr size_t chunk_size = 2 * 1024 * 1024;
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.is_regular_file() && entry.path().extension() == ".csv") {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        std::vector<Text> result;
        for (const auto& path : paths) {
            Text rows = load(path);
            while (rows.size() > chunk_size) {
                size_t newline = rows.find('\n', chunk_size);
                if (newline == Text::npos) break;
                result.push_back(rows.substr(0, newline + 1));
                rows.remove_prefix(newline + 1);
            }
            if (!rows.empty()) result.push_back(rows);
        }
        return result;
    }

private:
    std::vector<std::unique_ptr<MappedFile>> files_;
};

// Separate the structural bytes once per vector block. Commas always end
// fields, and only ASCII LF ends a row under the challenge dialect.
template <class Bits>
struct CsvMasks {
    Bits separators;
    Bits newlines;
    Bits quotes;
};

inline CsvMasks<uint32_t> csv_masks(const char* bytes) {
    const auto low = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes));
    const auto high = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + 16));
    auto mask = [&](char byte) -> uint32_t {
        const auto needle = _mm_set1_epi8(byte);
        return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(low, needle))) |
               (static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(high, needle))) << 16);
    };
    const uint32_t newlines = mask('\n');
    return {mask(',') | newlines, newlines, mask('"')};
}

[[gnu::target("avx2")]]
inline CsvMasks<uint32_t> csv_masks_avx2(const char* bytes) {
    const auto block = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes));
    const auto comma = _mm256_cmpeq_epi8(block, _mm256_set1_epi8(','));
    const auto newline = _mm256_cmpeq_epi8(block, _mm256_set1_epi8('\n'));
    const auto quote = _mm256_cmpeq_epi8(block, _mm256_set1_epi8('"'));
    return {static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_or_si256(comma, newline))),
            static_cast<uint32_t>(_mm256_movemask_epi8(newline)),
            static_cast<uint32_t>(_mm256_movemask_epi8(quote))};
}

[[gnu::target("avx512bw")]]
inline CsvMasks<uint64_t> csv_masks_avx512(const char* bytes) {
    const auto block = _mm512_loadu_si512(bytes);
    const uint64_t comma = _mm512_cmpeq_epi8_mask(block, _mm512_set1_epi8(','));
    const uint64_t newline = _mm512_cmpeq_epi8_mask(block, _mm512_set1_epi8('\n'));
    const uint64_t quote = _mm512_cmpeq_epi8_mask(block, _mm512_set1_epi8('"'));
    return {comma | newline, newline, quote};
}

template <size_t N, size_t Width, class Callback, class ReadMasks>
[[gnu::always_inline]]
inline void scan_csv(Text bytes, Callback&& callback, ReadMasks read_masks) {
    if (bytes.empty()) return;
    const char* field_start = bytes.data();
    size_t quotes_so_far = 0;
    size_t count = 0;
    bool valid = true;
    std::array<Text, N> fields;
    const size_t length = bytes.size() + (bytes.back() != '\n');
    std::array<char, Width> tail{};
    for (size_t offset = 0; offset < length; offset += Width) {
        const char* block = bytes.data() + offset;
        const size_t available = bytes.size() - offset;
        if (available < Width) {
            std::memcpy(tail.data(), block, available);
            if (available + offset < length) tail[available] = '\n';
        }
        auto masks = read_masks(available < Width ? tail.data() : block);
        using Bits = decltype(masks.separators);
        while (masks.separators) {
            const unsigned position = std::countr_zero(masks.separators);
            const Bits bit = Bits{1} << position;
            const Bits before = masks.quotes & (bit - 1);
            const char* end = block + position;
            const size_t quotes = quotes_so_far + (before ? std::popcount(before) : 0);
            const size_t size = end - field_start;
            if (count < N) {
                if (quotes == 0) {
                    fields[count] = {field_start, size};
                } else if (quotes == 2 && size >= 2 && *field_start == '"' && end[-1] == '"') {
                    fields[count] = {field_start + 1, size - 2};
                } else {
                    valid = false;
                }
                ++count;
            } else {
                valid = false;
            }
            if (masks.newlines & bit) {
                if (valid && count == N) callback(fields);
                count = 0;
                valid = true;
            }
            field_start = end == bytes.data() + bytes.size() ? end : end + 1;
            quotes_so_far = 0;
            masks.quotes ^= before;
            masks.separators ^= bit;
        }
        if (masks.quotes) quotes_so_far += std::popcount(masks.quotes);
    }
}

template <size_t N, class Callback>
[[gnu::target("avx2,popcnt,bmi")]]
void scan_csv_avx2(Text bytes, Callback&& callback) {
    scan_csv<N, 32>(bytes, std::forward<Callback>(callback), csv_masks_avx2);
}

template <size_t N, class Callback>
[[gnu::target("avx512bw,popcnt,bmi")]]
void scan_csv_avx512(Text bytes, Callback&& callback) {
    scan_csv<N, 64>(bytes, std::forward<Callback>(callback), csv_masks_avx512);
}

template <size_t N, class Callback>
void csv_rows(Text bytes, Callback&& callback) {
    const bool bit_ops = __builtin_cpu_supports("popcnt") && __builtin_cpu_supports("bmi");
    if (bit_ops && __builtin_cpu_supports("avx512bw")) {
        scan_csv_avx512<N>(bytes, std::forward<Callback>(callback));
    } else if (bit_ops && __builtin_cpu_supports("avx2")) {
        scan_csv_avx2<N>(bytes, std::forward<Callback>(callback));
    } else {
        scan_csv<N, 32>(bytes, std::forward<Callback>(callback), csv_masks);
    }
}

inline int two_digits(const char* p) {
    unsigned first = static_cast<unsigned char>(p[0]) - '0';
    unsigned second = static_cast<unsigned char>(p[1]) - '0';
    return first <= 9 && second <= 9 ? static_cast<int>(first * 10 + second) : -1;
}

inline int64_t unsigned_value(Text text, int64_t max) {
    if (text.empty()) return -1;
    while (text.size() > 1 && text.front() == '0') text.remove_prefix(1);
    int64_t value = 0;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (text.front() < '0' || text.front() > '9' || result.ec != std::errc{} ||
        result.ptr != end || value > max) {
        return -1;
    }
    return value;
}

struct Calendar {
    struct Month {
        int32_t first_day;
        int32_t days;
    };
    std::array<Month, 10'000 * 12> months;

    Calendar() {
        using namespace std::chrono;
        for (int y = 0; y < 10'000; ++y) {
            for (unsigned m = 1; m <= 12; ++m) {
                const auto first = sys_days(year(y) / month(m) / day(1));
                const auto last = (year(y) / month(m) / std::chrono::last).day();
                months[y * 12 + m - 1] = {static_cast<int32_t>(first.time_since_epoch().count()),
                                           static_cast<int32_t>(unsigned(last))};
            }
        }
    }
};

inline const Calendar& calendar() {
    static const Calendar days;
    return days;
}

using DateDigits = std::array<int16_t, 8>;

[[gnu::target("ssse3")]]
inline DateDigits iso_digits_simd(const char* bytes) {
    const auto first = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes));
    const auto last = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bytes + 4));
    const auto from_first = _mm_setr_epi8(0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, -1, -1, -1, -1);
    const auto from_last = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 13, 14, -1, -1);
    const auto ascii = _mm_or_si128(_mm_shuffle_epi8(first, from_first),
                                   _mm_shuffle_epi8(last, from_last));
    const auto digits = _mm_sub_epi8(ascii, _mm_set1_epi8('0'));
    const auto invalid = _mm_or_si128(_mm_cmpgt_epi8(digits, _mm_set1_epi8(9)),
                                     _mm_cmpgt_epi8(_mm_setzero_si128(), digits));
    if ((_mm_movemask_epi8(invalid) & 0x3fff) != 0) return {-1};
    const auto numbers = _mm_maddubs_epi16(digits, _mm_set1_epi16(0x010a));
    DateDigits result;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(result.data()), numbers);
    return result;
}

inline DateDigits iso_digits(const char* bytes) {
    if (__builtin_cpu_supports("ssse3")) return iso_digits_simd(bytes);
    return {static_cast<int16_t>(two_digits(bytes)),
            static_cast<int16_t>(two_digits(bytes + 2)),
            static_cast<int16_t>(two_digits(bytes + 5)),
            static_cast<int16_t>(two_digits(bytes + 8)),
            static_cast<int16_t>(two_digits(bytes + 11)),
            static_cast<int16_t>(two_digits(bytes + 14)),
            static_cast<int16_t>(two_digits(bytes + 17))};
}

inline int64_t iso_timestamp(Text text) {
    if ((text.size() != 20 && text.size() != 25) || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return invalid_ts;
    }
    const char* p = text.data();
    const auto [century, decade, mm, dd, hh, min, sec, unused] = iso_digits(p);
    if (century < 0 || decade < 0 || mm < 1 || mm > 12 || dd < 1 ||
        hh < 0 || hh > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        return invalid_ts;
    }
    const auto& month = calendar().months[(century * 100 + decade) * 12 + mm - 1];
    if (dd > month.days) return invalid_ts;
    int offset = 0;
    if (text.size() == 20) {
        if (p[19] != 'Z') return invalid_ts;
    } else {
        if ((p[19] != '+' && p[19] != '-') || p[22] != ':') return invalid_ts;
        const int hours = two_digits(p + 20);
        const int minutes = two_digits(p + 23);
        if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return invalid_ts;
        offset = (hours * 60 + minutes) * (p[19] == '+' ? 1 : -1);
    }
    return (month.first_day + dd - 1) * day_ms +
           ((hh * 60 + min - offset) * 60 + sec) * 1000;
}

// Parse the common 13-digit epoch form with parallel decimal reductions.
inline int64_t epoch_thirteen(const char* bytes) {
    uint64_t first;
    uint32_t middle;
    std::memcpy(&first, bytes, sizeof(first));
    std::memcpy(&middle, bytes + 8, sizeof(middle));
    const unsigned last = static_cast<unsigned char>(bytes[12]) - '0';
    if (((first | (first + 0x4646464646464646ULL) |
          (first - 0x3030303030303030ULL)) & 0x8080808080808080ULL) ||
        ((middle | (middle + 0x46464646U) |
          (middle - 0x30303030U)) & 0x80808080U) || last > 9) {
        return invalid_ts;
    }
    first &= 0x0f0f0f0f0f0f0f0fULL;
    first = (first * 10 + (first >> 8)) & 0x00ff00ff00ff00ffULL;
    first = (first * 100 + (first >> 16)) & 0x0000ffff0000ffffULL;
    const uint32_t prefix = static_cast<uint32_t>(first * 10'000 + (first >> 32));
    middle &= 0x0f0f0f0fU;
    middle = (middle * 10 + (middle >> 8)) & 0x00ff00ffU;
    const uint32_t suffix = (middle * 100 + (middle >> 16)) & 0x0000ffffU;
    return static_cast<int64_t>(prefix) * 100'000 + suffix * 10 + last;
}

inline int64_t timestamp(Text text) {
    if (text.size() == 13) return epoch_thirteen(text.data());
    if ((text.size() == 20 || text.size() == 25) && text[4] == '-') {
        return iso_timestamp(text);
    }
    const int64_t value = unsigned_value(text, std::numeric_limits<int64_t>::max());
    return value < 0 ? invalid_ts : value;
}

inline int32_t utc_day(int64_t millis) {
    const auto day = millis / day_ms;
    return static_cast<int32_t>(day - (millis % day_ms < 0));
}

struct Window {
    int64_t start;
    int64_t end;
};

inline Window read_window(const fs::path& path) {
    std::ifstream input(path);
    const std::string manifest{std::istreambuf_iterator<char>(input), {}};
    auto read_bound = [&](Text key) {
        const auto key_pos = manifest.find(key);
        if (key_pos == Text::npos) throw std::runtime_error("Missing manifest key");
        const auto colon = manifest.find(':', key_pos + key.size());
        const auto open = colon == Text::npos ? Text::npos : manifest.find('"', colon + 1);
        const auto end = open == Text::npos ? Text::npos : manifest.find('"', open + 1);
        if (end == Text::npos) throw std::runtime_error("Invalid manifest value");
        return iso_timestamp(Text(manifest).substr(open + 1, end - open - 1));
    };
    Window window{read_bound("\"window_start\""), read_bound("\"window_end\"")};
    if (window.start == invalid_ts || window.end == invalid_ts || window.end <= window.start) {
        throw std::runtime_error("Invalid manifest window");
    }
    return window;
}

}  // namespace shopping
