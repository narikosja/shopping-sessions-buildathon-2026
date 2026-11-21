#include "input.hpp"
#include "key.hpp"

#include <cstdlib>
#include <iostream>
#include <random>

template<size_t Columns>
void verify_csv(Text chunk) {
    std::vector<std::array<Text, Columns>> expected;
    for_lines(chunk, [&](Text line) {
        std::array<Text, Columns> fields;
        if (split_csv(line, fields)) expected.push_back(fields);
    });
    size_t next = 0;
    for_csv<Columns>(chunk, [&](const auto& fields) {
        if (next == expected.size() || expected[next++] != fields) std::abort();
    });
    if (next != expected.size()) std::abort();
}

std::string csv_row(std::mt19937& random, bool structured, unsigned columns) {
    std::string line;
    if (structured) {
        for (unsigned c = 0; c < columns; ++c) {
            if (c) line.push_back(',');
            const bool quoted = random() % 2;
            if (quoted) line.push_back('"');
            for (size_t j = random() % 29; j > 0; --j) line.push_back("abc n\r"[random() % 6]);
            if (quoted) line.push_back('"');
        }
    } else {
        const Text alphabet = "abcXYZ019 ,-\r\t\"";
        for (size_t j = random() % 180; j > 0; --j) line.push_back(alphabet[random() % alphabet.size()]);
    }
    return line;
}

int main() {
    std::mt19937 random(423278);
    TimestampParser times;
    for (unsigned batch = 0; batch < 2000; ++batch) {
        std::string chunk;
        KeyArena first, second;
        for (unsigned i = 0; i < 500; ++i) {
            if (i) chunk.push_back('\n');
            chunk += csv_row(random, i % 2, (i % 4) ? 7 : 4);

            std::string compact;
            for (size_t j = random() % 15; j > 0; --j) compact.push_back(static_cast<char>(random() % 128));
            const auto a = first.make(compact);
            const auto b = second.make(compact);
            std::array<char, 9> buffer;
            if (a.text(buffer) != compact || a != b || a.hash() != b.hash()) std::abort();

            std::string date = std::to_string(1000000000000ULL +
                (uint64_t{random()} * 1024 + random() % 999) % 8999999999999ULL);
            if (i % 2) date[random() % date.size()] = static_cast<char>(random() % 256);
            if (times.parse(date) != timestamp(date)) std::abort();
        }
        if (batch % 2) chunk.push_back('\n');
        verify_csv<4>(chunk);
        verify_csv<7>(chunk);
    }
    for (const Text value : {"2028-02-29T23:59:59Z", "2028-03-01T00:00:00Z",
                             "2027-02-29T23:59:59Z", "1969-12-31T23:59:59Z",
                             "1969-12-31T23:59:59-12:00", "1970-01-01T00:00:00Z",
                             "2000-02-29T12:34:56Z", "1900-02-29T12:34:56Z",
                             "2028-01-01T24:00:00Z", "2028-01-01T00:00:60Z"}) {
        const auto expected = timestamp(value);
        if (times.parse(value) != expected) std::abort();
        if (expected != invalid_time && value.size() == 20 && times.ingest(value) != expected) std::abort();
    }
    std::cout << "1,000,000 CSV rows, IDs, and timestamps matched the scalar reference\n";
}
