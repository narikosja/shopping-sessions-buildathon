#pragma once

#include <algorithm>
#include <array>
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

inline bool field(Text value, Text& out) {
    if (!value.empty() && value.front() == '"') {
        if (value.size() < 2 || value.back() != '"') return false;
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    if (value.find('"') != Text::npos) return false;
    out = value;
    return true;
}

template <size_t N, class Callback>
void csv_rows(Text bytes, Callback&& callback) {
    while (!bytes.empty()) {
        const auto newline = bytes.find('\n');
        Text line = bytes.substr(0, newline);
        bytes.remove_prefix(newline == Text::npos ? bytes.size() : newline + 1);
        std::array<Text, N> fields;
        size_t count = 0;
        for (; count < N - 1; ++count) {
            const auto comma = line.find(',');
            if (comma == Text::npos || !field(line.substr(0, comma), fields[count])) break;
            line.remove_prefix(comma + 1);
        }
        if (count == N - 1 && line.find(',') == Text::npos && field(line, fields.back())) {
            callback(fields);
        }
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

inline int64_t iso_timestamp(Text text) {
    using namespace std::chrono;
    if ((text.size() != 20 && text.size() != 25) || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return invalid_ts;
    }
    const char* p = text.data();
    const int century = two_digits(p);
    const int decade = two_digits(p + 2);
    const int mm = two_digits(p + 5);
    const int dd = two_digits(p + 8);
    const int hh = two_digits(p + 11);
    const int min = two_digits(p + 14);
    const int sec = two_digits(p + 17);
    if (century < 0 || decade < 0 || mm < 1 || dd < 1 ||
        hh < 0 || hh > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        return invalid_ts;
    }
    const year_month_day date{year(century * 100 + decade), month(mm), day(dd)};
    if (!date.ok()) return invalid_ts;
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
    return sys_days(date).time_since_epoch().count() * day_ms +
           ((hh * 60 + min - offset) * 60 + sec) * 1000;
}

inline int64_t timestamp(Text text) {
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
