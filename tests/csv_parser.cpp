#include "input.hpp"

#include <cstdlib>
#include <iostream>
#include <random>

template<size_t n>
void verify(Text line) {
    std::array<Text, n> expected{}, actual{};
    const bool valid = split_csv_scalar(line, expected);
    if (split_csv(line, actual) != valid || (valid && actual != expected)) std::abort();
}

int main() {
    std::mt19937 random(423278);
    const Text alphabet = "abcXYZ019 ,-\r\t\"";
    for (size_t iteration = 0; iteration < 1000000; ++iteration) {
        std::string line;
        if (iteration % 2) {
            for (size_t j = random() % 180; j > 0; --j) line.push_back(alphabet[random() % alphabet.size()]);
        } else {
            const unsigned columns = (iteration % 4) ? 7 : 4;
            for (unsigned c = 0; c < columns; ++c) {
                if (c) line.push_back(',');
                const bool quoted = random() % 2;
                if (quoted) line.push_back('"');
                for (size_t j = random() % 30; j > 0; --j) line.push_back("abc n\r"[random() % 6]);
                if (quoted) line.push_back('"');
            }
        }
        verify<4>(line);
        verify<7>(line);
    }
    std::cout << "1,000,000 CSV lines matched the scalar parser\n";
}
