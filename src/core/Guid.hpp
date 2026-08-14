#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ege {

    // A 128-bit identifier, printed as 32 lowercase hex digits.
    //
    // This is what makes an asset reference survive a file being moved or
    // renamed: the reference names the id, the id lives in a sidecar beside
    // the file, and a path is only ever the answer to "where is it now".
    // Storing paths instead is the mistake every asset pipeline makes once.
    //
    // 128 bits rather than 64 because ids are minted independently on
    // different machines and merged by version control, where a collision is
    // not a crash but two assets quietly becoming one.
    class Guid {
    public:
        constexpr Guid() = default;

        constexpr Guid(std::uint64_t highRef, std::uint64_t lowRef)
            : highBits{highRef}, lowBits{lowRef} {}

        // A fresh random id. Thread-safe.
        static Guid generate();

        // A deterministic id for something that has no file to keep a sidecar
        // beside: a procedural mesh, a built-in material. The same name always
        // gives the same id, on every machine and every run, which is what
        // lets a scene reference an asset the code creates rather than loads.
        static Guid fromName(std::string_view name);

        // Parses 32 hex digits, with or without the conventional dashes.
        // Nothing else - a malformed id is a corrupt file, not a null asset.
        static std::optional<Guid> parse(std::string_view text);

        std::string toString() const;

        constexpr bool isNull() const { return highBits == 0 && lowBits == 0; }

        constexpr std::uint64_t high() const { return highBits; }

        constexpr std::uint64_t low() const { return lowBits; }

        friend constexpr bool operator==(Guid a, Guid b) {
            return a.highBits == b.highBits && a.lowBits == b.lowBits;
        }

        friend constexpr bool operator!=(Guid a, Guid b) { return !(a == b); }

        // Ordering so ids can key a sorted container; no meaning beyond that.
        friend constexpr bool operator<(Guid a, Guid b) {
            return a.highBits != b.highBits ? a.highBits < b.highBits : a.lowBits < b.lowBits;
        }

    private:
        std::uint64_t highBits = 0;
        std::uint64_t lowBits = 0;
    };

}  // namespace ege

template<>
struct std::hash<ege::Guid> {
    std::size_t operator()(ege::Guid id) const noexcept {
        // The bits are already well mixed, so folding the halves is enough.
        return static_cast<std::size_t>(id.high() ^ (id.low() * 0x9e3779b97f4a7c15ull));
    }
};
