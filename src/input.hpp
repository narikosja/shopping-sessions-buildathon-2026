#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Text = std::string_view;
using Time = int64_t;
constexpr Time day_ms = 86400000;
constexpr Time week_ms = 7 * day_ms;
constexpr Time session_gap_ms = 1800000;
constexpr Time invalid_time = std::numeric_limits<Time>::min();

class Mapping {
public:
    explicit Mapping(const fs::path& path) {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), path.string());
        const auto size = lseek(fd, 0, SEEK_END);
        if (size < 0) {
            const int error = errno;
            close(fd);
            throw std::system_error(error, std::generic_category(), path.string());
        }
        if (!size) {
            close(fd);
            return;
        }
        auto* address = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        const int error = errno;
        close(fd);
        if (address == MAP_FAILED) throw std::system_error(error, std::generic_category(), path.string());
        data_ = Text(static_cast<const char*>(address), size);
    }

    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& other) noexcept : data_(std::exchange(other.data_, {})) {}
    ~Mapping() {
        if (!data_.empty()) munmap(const_cast<char*>(data_.data()), data_.size());
    }
    Text data() const { return data_; }

private:
    Text data_;
};

struct InputParts {
    std::vector<Mapping> files;
    std::vector<Text> chunks;
    size_t bytes = 0;

    explicit InputParts(const fs::path& dir) {
        if (!fs::exists(dir)) return;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".csv") {
                files.emplace_back(entry.path());
                Text data = files.back().data();
                const auto header = data.find('\n');
                if (header == Text::npos) continue;
                data.remove_prefix(header + 1);
                bytes += data.size();
                constexpr size_t chunk_size = 2 * 1024 * 1024;
                while (data.size() > chunk_size) {
                    const auto newline = data.find('\n', chunk_size - 1);
                    if (newline == Text::npos) break;
                    chunks.push_back(data.substr(0, newline + 1));
                    data.remove_prefix(newline + 1);
                }
                if (!data.empty()) chunks.push_back(data);
            }
        }
    }
};

template<class Function>
void for_lines(Text chunk, Function visit) {
    while (!chunk.empty()) {
        const auto newline = chunk.find('\n');
        if (newline == Text::npos) {
            visit(chunk);
            break;
        }
        visit(chunk.substr(0, newline));
        chunk.remove_prefix(newline + 1);
    }
}

inline Text after_header(Text file) {
    const auto newline = file.find('\n');
    if (newline == Text::npos) return {};
    return file.substr(newline + 1);
}

template<size_t Columns>
bool split_csv(Text line, std::array<Text, Columns>& fields) {
    const char* begin = line.data();
    const char* end = begin + line.size();
    for (size_t col = 0; col < Columns; ++col) {
        const char* comma = end;
        if (col + 1 != Columns) {
            comma = static_cast<const char*>(std::memchr(begin, ',', end - begin));
            if (!comma) return false;
        } else if (std::memchr(begin, ',', end - begin)) {
            return false;
        }
        Text value(begin, comma - begin);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        if (value.find('"') != Text::npos) return false;
        fields[col] = value;
        if (col + 1 != Columns) begin = comma + 1;
    }
    return true;
}

inline int digits2(const char* text) {
    const unsigned a = static_cast<unsigned char>(text[0]) - '0';
    const unsigned b = static_cast<unsigned char>(text[1]) - '0';
    if (a > 9 || b > 9) return -1;
    return a * 10 + b;
}

inline Time unsigned_decimal(Text text, Time max_value) {
    if (text.empty()) return invalid_time;
    Time result = 0;
    for (unsigned char c : text) {
        const unsigned digit = c - '0';
        if (digit > 9 || result > (max_value - digit) / 10) return invalid_time;
        result = result * 10 + digit;
    }
    return result;
}

inline Time timestamp(Text text) {
    if (text.size() < 20 || text[4] != '-') {
        return unsigned_decimal(text, std::numeric_limits<Time>::max());
    }

    const auto size = text.size();
    if ((size != 20 && size != 25) || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') return invalid_time;
    const int century = digits2(text.data());
    const int year_part = digits2(text.data() + 2);
    const int month = digits2(text.data() + 5);
    const int day = digits2(text.data() + 8);
    const int hour = digits2(text.data() + 11);
    const int minute = digits2(text.data() + 14);
    const int second = digits2(text.data() + 17);
    if (century < 0 || year_part < 0 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) return invalid_time;
    using namespace std::chrono;
    const year_month_day civil{year{century * 100 + year_part},
                               std::chrono::month{static_cast<unsigned>(month)},
                               std::chrono::day{static_cast<unsigned>(day)}};
    if (!civil.ok()) return invalid_time;
    int offset = 0;
    if (size == 20) {
        if (text[19] != 'Z') return invalid_time;
    } else {
        if ((text[19] != '-' && text[19] != '+') || text[22] != ':') return invalid_time;
        const int h = digits2(text.data() + 20);
        const int m = digits2(text.data() + 23);
        if (h < 0 || h > 23 || m < 0 || m > 59) return invalid_time;
        offset = (h * 60 + m) * (text[19] == '+' ? 1 : -1);
    }
    return sys_days(civil).time_since_epoch().count() * day_ms +
           ((hour * 60 + minute - offset) * 60 + second) * 1000;
}

inline int32_t utc_day(Time time) {
    return static_cast<int32_t>(time / day_ms - (time % day_ms < 0));
}

struct Window {
    Time begin;
    Time end;
};

inline Window window_from_manifest(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    const std::string data((std::istreambuf_iterator<char>(input)), {});
    auto read = [&](Text key) {
        const auto key_at = data.find(key);
        if (key_at == Text::npos) throw std::runtime_error("Missing manifest key");
        const auto colon = data.find(':', key_at + key.size());
        const auto opening = data.find('"', colon);
        if (opening == Text::npos) throw std::runtime_error("Missing manifest timestamp");
        const auto closing = data.find('"', opening + 1);
        if (closing == Text::npos) throw std::runtime_error("Unterminated manifest timestamp");
        return timestamp(Text(data).substr(opening + 1, closing - opening - 1));
    };
    Window window{read("\"window_start\""), read("\"window_end\"")};
    if (window.begin == invalid_time || window.end == invalid_time || window.begin >= window.end) {
        throw std::runtime_error("Invalid manifest window");
    }
    return window;
}
