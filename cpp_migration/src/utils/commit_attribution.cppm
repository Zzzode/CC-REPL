module;

#include <array>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.utils.commit_attribution;

export namespace cc::utils::commit_attribution {

struct FileAttributionState {
    std::string content_hash;
    std::size_t claude_contribution = 0;
    double mtime = 0.0;
};

struct AttributionState {
    std::map<std::string, FileAttributionState> file_states;
    std::map<std::string, FileAttributionState> session_baselines;
    std::string surface = "cli";
    std::string starting_head_sha;
    int prompt_count = 0;
    int prompt_count_at_last_commit = 0;
    int permission_prompt_count = 0;
    int permission_prompt_count_at_last_commit = 0;
    int escape_count = 0;
    int escape_count_at_last_commit = 0;
};

enum class FileChangeType {
    Modified,
    Created,
    Deleted,
};

struct FileChange {
    std::string path;
    FileChangeType type = FileChangeType::Modified;
    std::string old_content;
    std::string new_content;
    double mtime = 0.0;
};

namespace detail {
    [[nodiscard]] inline bool contains(std::string_view haystack, std::string_view needle) noexcept {
        return haystack.find(needle) != std::string_view::npos;
    }

    [[nodiscard]] inline std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) noexcept {
        return (value >> bits) | (value << (32u - bits));
    }

    [[nodiscard]] inline std::string sha256(std::string_view content) {
        constexpr std::array<std::uint32_t, 64> k = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        std::vector<unsigned char> bytes(content.begin(), content.end());
        const std::uint64_t bit_len = static_cast<std::uint64_t>(bytes.size()) * 8u;
        bytes.push_back(0x80u);
        while ((bytes.size() % 64u) != 56u) bytes.push_back(0u);
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<unsigned char>((bit_len >> shift) & 0xffu));
        }

        std::uint32_t h0 = 0x6a09e667u;
        std::uint32_t h1 = 0xbb67ae85u;
        std::uint32_t h2 = 0x3c6ef372u;
        std::uint32_t h3 = 0xa54ff53au;
        std::uint32_t h4 = 0x510e527fu;
        std::uint32_t h5 = 0x9b05688cu;
        std::uint32_t h6 = 0x1f83d9abu;
        std::uint32_t h7 = 0x5be0cd19u;

        for (std::size_t chunk = 0; chunk < bytes.size(); chunk += 64) {
            std::array<std::uint32_t, 64> w{};
            for (std::size_t i = 0; i < 16; ++i) {
                const std::size_t j = chunk + i * 4;
                w[i] = (static_cast<std::uint32_t>(bytes[j]) << 24u) |
                       (static_cast<std::uint32_t>(bytes[j + 1]) << 16u) |
                       (static_cast<std::uint32_t>(bytes[j + 2]) << 8u) |
                       static_cast<std::uint32_t>(bytes[j + 3]);
            }
            for (std::size_t i = 16; i < 64; ++i) {
                const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3u);
                const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10u);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            std::uint32_t a = h0;
            std::uint32_t b = h1;
            std::uint32_t c = h2;
            std::uint32_t d = h3;
            std::uint32_t e = h4;
            std::uint32_t f = h5;
            std::uint32_t g = h6;
            std::uint32_t h = h7;

            for (std::size_t i = 0; i < 64; ++i) {
                const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                const std::uint32_t ch = (e & f) ^ ((~e) & g);
                const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
                const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t temp2 = s0 + maj;

                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            h0 += a;
            h1 += b;
            h2 += c;
            h3 += d;
            h4 += e;
            h5 += f;
            h6 += g;
            h7 += h;
        }

        std::ostringstream out;
        out << std::hex << std::setfill('0')
            << std::setw(8) << h0 << std::setw(8) << h1 << std::setw(8) << h2 << std::setw(8) << h3
            << std::setw(8) << h4 << std::setw(8) << h5 << std::setw(8) << h6 << std::setw(8) << h7;
        return out.str();
    }

    [[nodiscard]] inline std::vector<std::uint32_t> utf16_units(std::string_view utf8) {
        std::vector<std::uint32_t> units;
        for (std::size_t i = 0; i < utf8.size();) {
            const auto byte = static_cast<unsigned char>(utf8[i]);
            std::uint32_t cp = 0xfffdu;
            std::size_t consumed = 1;
            if (byte < 0x80u) {
                cp = byte;
            } else if ((byte & 0xe0u) == 0xc0u && i + 1 < utf8.size()) {
                cp = ((byte & 0x1fu) << 6u) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3fu);
                consumed = 2;
            } else if ((byte & 0xf0u) == 0xe0u && i + 2 < utf8.size()) {
                cp = ((byte & 0x0fu) << 12u) |
                     ((static_cast<unsigned char>(utf8[i + 1]) & 0x3fu) << 6u) |
                     (static_cast<unsigned char>(utf8[i + 2]) & 0x3fu);
                consumed = 3;
            } else if ((byte & 0xf8u) == 0xf0u && i + 3 < utf8.size()) {
                cp = ((byte & 0x07u) << 18u) |
                     ((static_cast<unsigned char>(utf8[i + 1]) & 0x3fu) << 12u) |
                     ((static_cast<unsigned char>(utf8[i + 2]) & 0x3fu) << 6u) |
                     (static_cast<unsigned char>(utf8[i + 3]) & 0x3fu);
                consumed = 4;
            }

            if (cp > 0xffffu) {
                units.push_back(0xd800u + ((cp - 0x10000u) >> 10u));
                units.push_back(0xdc00u + ((cp - 0x10000u) & 0x3ffu));
            } else {
                units.push_back(cp);
            }
            i += consumed;
        }
        return units;
    }

    [[nodiscard]] inline std::size_t changed_region_size(std::string_view old_content, std::string_view new_content) {
        const auto old_units = utf16_units(old_content);
        const auto new_units = utf16_units(new_content);
        if (old_units.empty() || new_units.empty()) return old_units.empty() ? new_units.size() : old_units.size();

        const std::size_t min_len = std::min(old_units.size(), new_units.size());
        std::size_t prefix_end = 0;
        while (prefix_end < min_len && old_units[prefix_end] == new_units[prefix_end]) {
            ++prefix_end;
        }

        std::size_t suffix_len = 0;
        while (suffix_len < min_len - prefix_end &&
               old_units[old_units.size() - 1 - suffix_len] == new_units[new_units.size() - 1 - suffix_len]) {
            ++suffix_len;
        }

        const std::size_t old_changed_len = old_units.size() - prefix_end - suffix_len;
        const std::size_t new_changed_len = new_units.size() - prefix_end - suffix_len;
        return std::max(old_changed_len, new_changed_len);
    }

    [[nodiscard]] inline FileAttributionState compute_file_modification_state(
        const std::map<std::string, FileAttributionState>& existing_file_states,
        std::string_view normalized_path,
        std::string_view old_content,
        std::string_view new_content,
        double mtime) {
        const auto existing = existing_file_states.find(std::string(normalized_path));
        const std::size_t existing_contribution = existing == existing_file_states.end() ? 0 : existing->second.claude_contribution;
        return FileAttributionState{
            .content_hash = sha256(new_content),
            .claude_contribution = existing_contribution + changed_region_size(old_content, new_content),
            .mtime = mtime,
        };
    }
} // namespace detail

[[nodiscard]] inline std::string sanitize_model_name(std::string_view short_name) {
    if (detail::contains(short_name, "opus-4-6")) return "claude-opus-4-6";
    if (detail::contains(short_name, "opus-4-5")) return "claude-opus-4-5";
    if (detail::contains(short_name, "opus-4-1")) return "claude-opus-4-1";
    if (detail::contains(short_name, "opus-4")) return "claude-opus-4";
    if (detail::contains(short_name, "sonnet-4-6")) return "claude-sonnet-4-6";
    if (detail::contains(short_name, "sonnet-4-5")) return "claude-sonnet-4-5";
    if (detail::contains(short_name, "sonnet-4")) return "claude-sonnet-4";
    if (detail::contains(short_name, "sonnet-3-7")) return "claude-sonnet-3-7";
    if (detail::contains(short_name, "haiku-4-5")) return "claude-haiku-4-5";
    if (detail::contains(short_name, "haiku-3-5")) return "claude-haiku-3-5";
    return "claude";
}

[[nodiscard]] inline std::string sanitize_surface_key(std::string_view surface_key) {
    const auto slash_index = surface_key.rfind('/');
    if (slash_index == std::string_view::npos) return std::string(surface_key);
    return std::string(surface_key.substr(0, slash_index)) + "/" + sanitize_model_name(surface_key.substr(slash_index + 1));
}

[[nodiscard]] inline AttributionState create_empty_attribution_state(std::string surface = "cli") {
    AttributionState state;
    state.surface = std::move(surface);
    return state;
}

[[nodiscard]] inline AttributionState track_file_modification(
    AttributionState state,
    std::string_view file_path,
    std::string_view old_content,
    std::string_view new_content,
    double mtime = 0.0) {
    const std::string normalized_path(file_path);
    state.file_states[normalized_path] = detail::compute_file_modification_state(state.file_states, normalized_path, old_content, new_content, mtime);
    return state;
}

[[nodiscard]] inline AttributionState track_file_creation(
    AttributionState state,
    std::string_view file_path,
    std::string_view content,
    double mtime = 0.0) {
    return track_file_modification(std::move(state), file_path, "", content, mtime);
}

[[nodiscard]] inline AttributionState track_file_deletion(
    AttributionState state,
    std::string_view file_path,
    std::string_view old_content,
    double mtime = 0.0) {
    const std::string normalized_path(file_path);
    const auto existing = state.file_states.find(normalized_path);
    const std::size_t existing_contribution = existing == state.file_states.end() ? 0 : existing->second.claude_contribution;
    state.file_states[normalized_path] = FileAttributionState{
        .content_hash = "",
        .claude_contribution = existing_contribution + old_content.size(),
        .mtime = mtime,
    };
    return state;
}

[[nodiscard]] inline AttributionState track_bulk_file_changes(AttributionState state, const std::vector<FileChange>& changes) {
    for (const auto& change : changes) {
        if (change.type == FileChangeType::Deleted) {
            state = track_file_deletion(std::move(state), change.path, change.old_content, change.mtime);
        } else {
            state = track_file_modification(std::move(state), change.path, change.old_content, change.new_content, change.mtime);
        }
    }
    return state;
}

} // namespace cc::utils::commit_attribution
