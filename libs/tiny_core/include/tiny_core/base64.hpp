#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tiny::base64 {

inline constexpr auto alphabet = std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

// Standard alphabet, padded. Never contains a NUL, so it survives C-string containers.
inline auto encode(std::span<const std::byte> bytes) -> std::string
{
    auto out = std::string{};
    out.reserve((bytes.size() + 2) / 3 * 4);

    auto i = std::size_t{};
    for (; i + 3 <= bytes.size(); i += 3) {
        const auto n = (std::to_integer<std::uint32_t>(bytes[i]) << 16)
            | (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8)
            | std::to_integer<std::uint32_t>(bytes[i + 2]);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }

    if (const auto rest = bytes.size() - i; rest > 0) {
        auto n = std::to_integer<std::uint32_t>(bytes[i]) << 16;
        if (rest == 2) n |= std::to_integer<std::uint32_t>(bytes[i + 1]) << 8;
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += (rest == 2) ? alphabet[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// Strict: padded input only, no whitespace. Nullopt on anything malformed.
inline auto decode(std::string_view text) -> std::optional<std::vector<std::byte>>
{
    if (text.size() % 4 != 0) return std::nullopt;

    constexpr auto table = [] {
        auto t = std::array<std::int8_t, 256>{};
        t.fill(-1);
        for (auto i = std::size_t{}; i < alphabet.size(); ++i) {
            t[static_cast<unsigned char>(alphabet[i])] = static_cast<std::int8_t>(i);
        }
        return t;
    }();

    auto out = std::vector<std::byte>{};
    out.reserve(text.size() / 4 * 3);

    for (auto i = std::size_t{}; i < text.size(); i += 4) {
        const auto last = (i + 4 == text.size());
        const auto pad = (last && text[i + 3] == '=') ? ((text[i + 2] == '=') ? 2 : 1) : 0;

        auto n = std::uint32_t{};
        for (auto j = 0; j < 4 - pad; ++j) {
            const auto v = table[static_cast<unsigned char>(text[i + static_cast<std::size_t>(j)])];
            if (v < 0) return std::nullopt;
            n |= static_cast<std::uint32_t>(v) << (18 - 6 * j);
        }

        out.push_back(static_cast<std::byte>((n >> 16) & 0xff));
        if (pad < 2) out.push_back(static_cast<std::byte>((n >> 8) & 0xff));
        if (pad < 1) out.push_back(static_cast<std::byte>(n & 0xff));
    }
    return out;
}

} // namespace tiny::base64
