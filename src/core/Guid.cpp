#include "core/Guid.hpp"

#include <array>
#include <mutex>
#include <random>

namespace ege {

    namespace {

        // One engine behind a lock rather than a thread_local one. Ids are
        // minted when an asset is first seen, which is rare enough that the
        // lock never shows up, and a single seeded stream is far easier to
        // reason about than one per thread.
        std::mutex generatorMutex;

        std::mt19937_64& generator() {
            static std::mt19937_64 engine{std::random_device{}()};
            return engine;
        }

        int hexValue(char character) {
            if (character >= '0' && character <= '9') {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f') {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F') {
                return character - 'A' + 10;
            }
            return -1;
        }

        // FNV-1a, run twice over the same bytes with different offset bases to
        // fill 128 bits. Not a cryptographic hash and does not need to be: it
        // is naming, not signing, and the inputs are short engine-chosen
        // strings rather than anything an attacker supplies.
        std::uint64_t fnv1a(std::string_view text, std::uint64_t offsetBasis) {
            constexpr std::uint64_t prime = 1099511628211ull;
            std::uint64_t hash = offsetBasis;
            for (const char character : text) {
                hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
                hash *= prime;
            }
            return hash;
        }

    }  // namespace

    Guid Guid::generate() {
        const std::lock_guard<std::mutex> lock{generatorMutex};
        std::mt19937_64& engine = generator();
        for (;;) {
            const Guid candidate{engine(), engine()};
            // The null id means "no asset", so it cannot also mean an asset.
            // The odds are 2^-128; the branch is free and its absence would be
            // the kind of bug that surfaces once, years later.
            if (!candidate.isNull()) {
                return candidate;
            }
        }
    }

    Guid Guid::fromName(std::string_view name) {
        const Guid derived{fnv1a(name, 14695981039346656037ull), fnv1a(name, 1099511628211ull)};
        return derived.isNull() ? Guid{1, 1} : derived;
    }

    std::optional<Guid> Guid::parse(std::string_view text) {
        std::array<int, 32> digits{};
        std::size_t count = 0;
        for (const char character : text) {
            if (character == '-') {
                continue;
            }
            const int value = hexValue(character);
            if (value < 0 || count == digits.size()) {
                return std::nullopt;
            }
            digits[count++] = value;
        }
        if (count != digits.size()) {
            return std::nullopt;
        }

        std::uint64_t high = 0;
        std::uint64_t low = 0;
        for (std::size_t index = 0; index < 16; index++) {
            high = (high << 4) | static_cast<std::uint64_t>(digits[index]);
        }
        for (std::size_t index = 16; index < 32; index++) {
            low = (low << 4) | static_cast<std::uint64_t>(digits[index]);
        }
        return Guid{high, low};
    }

    std::string Guid::toString() const {
        constexpr char hexDigits[] = "0123456789abcdef";
        std::string out;
        out.resize(32);
        for (std::size_t index = 0; index < 16; index++) {
            const std::uint64_t shift = 60 - index * 4;
            out[index] = hexDigits[(highBits >> shift) & 0xF];
            out[index + 16] = hexDigits[(lowBits >> shift) & 0xF];
        }
        return out;
    }

}  // namespace ege
