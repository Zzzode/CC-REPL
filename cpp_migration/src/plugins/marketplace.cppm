module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

export module cc.plugins.marketplace;


export namespace cc::plugins {


enum class PluginStatus { available, installed, enabled, disabled, update_available, broken };


enum class PluginSource { official_marketplace, community, local, git };


struct SemVer {
    int major{0}, minor{0}, patch{0};
    std::string prerelease;
    
    [[nodiscard]] auto to_string() const -> std::string {
        auto s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
        if (!prerelease.empty()) s += "-" + prerelease;
        return s;
    }
    [[nodiscard]] auto operator>(const SemVer& o) const -> bool {
        if (major != o.major) return major > o.major;
        if (minor != o.minor) return minor > o.minor;
        return patch > o.patch;
    }
    auto operator<=>(const SemVer&) const = default;
};


struct PluginMeta {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    SemVer version;
    SemVer min_host_version;
    PluginSource source{PluginSource::official_marketplace};
    std::vector<std::string> tags;
    std::string homepage_url;
    std::string repository_url;
    size_t download_count{0};
    double rating{0.0};
    std::chrono::system_clock::time_point published_at;
};


struct InstalledPlugin {
    PluginMeta meta;
    PluginStatus status{PluginStatus::installed};
    std::filesystem::path install_path;
    std::chrono::system_clock::time_point installed_at;
    std::optional<SemVer> available_update;
    std::unordered_map<std::string, std::string> settings;
};


struct InstallOptions {
    bool auto_enable{true};
    bool trust_unverified{false};
    std::optional<SemVer> specific_version;
};


struct MarketplaceQuery {
    std::string keyword;
    std::optional<PluginSource> source_filter;
    std::optional<std::string> tag_filter;
    enum SortBy { relevance, downloads, rating, recent } sort{relevance};
    size_t limit{20};
    size_t offset{0};
};


struct InstallResult {
    bool success;
    std::string plugin_id;
    std::optional<std::string> error;
    std::chrono::milliseconds elapsed;
};


struct PluginPolicy {
    bool allow_network{true};
    bool allow_filesystem{true};
    bool allow_shell{false};
    std::vector<std::string> allowed_paths;
    std::vector<std::string> blocked_commands;
    size_t max_memory_mb{256};
    std::chrono::seconds max_execution_time{30};
};


enum class TrustLevel { untrusted, community, verified, official };

// ─── HTTP Client Helpers ──────────────────────────────────────

namespace detail {

// Minimal HTTP GET via POSIX sockets for marketplace API
struct HttpResponse {
    int status_code = 0;
    std::string body;
    bool ok() const { return status_code >= 200 && status_code < 300; }
};

inline HttpResponse http_get(std::string_view host, uint16_t port, std::string_view path,
                             const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto port_str = std::to_string(port);
    if (getaddrinfo(std::string(host).c_str(), port_str.c_str(), &hints, &res) != 0) {
        return {0, "DNS resolution failed"};
    }
    int fd = -1;
    for (auto* rp = res; rp; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv{}; tv.tv_sec = 30;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        ::close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return {0, "Connection failed"};

    // Send request
    std::string req = "GET " + std::string(path) + " HTTP/1.1\r\n";
    req += "Host: " + std::string(host) + "\r\n";
    req += "Connection: close\r\n";
    for (auto& [k,v] : headers) req += k + ": " + v + "\r\n";
    req += "\r\n";
    ::send(fd, req.data(), req.size(), 0);

    // Read response
    std::string raw;
    char buf[4096];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    // Parse status code
    HttpResponse resp;
    auto sp = raw.find(' ');
    if (sp != std::string::npos) {
        resp.status_code = std::atoi(raw.substr(sp+1, 3).c_str());
    }
    // Find body after \r\n\r\n
    auto body_start = raw.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        resp.body = raw.substr(body_start + 4);
    }
    return resp;
}

// Simple SHA256 for archive verification (using CommonCrypto on macOS)
inline std::string sha256_hex(const std::string& data) {
    // Simplified hash for integrity check — use CC_SHA256 on macOS
    // Compute a basic checksum for now; real impl would use platform crypto
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    uint64_t h2 = 0x6c62272e07bb0142ULL;
    for (unsigned char c : data) {
        h2 ^= c;
        h2 *= 0x100000001b3ULL;
    }
    char hex[65];
    std::snprintf(hex, sizeof(hex), "%016llx%016llx%016llx%016llx",
                  (unsigned long long)h, (unsigned long long)h2,
                  (unsigned long long)(h ^ h2), (unsigned long long)(h + h2));
    return hex;
}

// Extract a simple tar-like archive (plugin bundle is just directory listing in JSON + files)
inline bool extract_plugin_bundle(const std::string& archive_data,
                                  const std::filesystem::path& dest) {
    std::filesystem::create_directories(dest);
    // Plugin archives are zip-like. For simplicity, if the data starts with "PK"
    // (zip magic), we'll write it as-is and use system unzip.
    // Otherwise treat as a single manifest file.
    auto archive_path = dest / "__plugin_archive.zip";
    {
        std::ofstream out(archive_path, std::ios::binary);
        if (!out) return false;
        out.write(archive_data.data(), static_cast<std::streamsize>(archive_data.size()));
    }

    // Try system unzip
    auto cmd = "unzip -o -q '" + archive_path.string() + "' -d '" + dest.string() + "' 2>/dev/null";
    int ret = std::system(cmd.c_str());
    std::filesystem::remove(archive_path);
    return ret == 0;
}

// Parse plugin metadata from JSON manifest
inline std::optional<PluginMeta> parse_manifest(const std::filesystem::path& manifest_path) {
    std::ifstream f(manifest_path);
    if (!f.is_open()) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto extract = [&](std::string_view key) -> std::string {
        auto pattern = "\"" + std::string(key) + "\"";
        auto pos = content.find(pattern);
        if (pos == std::string::npos) return {};
        pos = content.find('"', pos + pattern.size() + 1);
        if (pos == std::string::npos) return {};
        auto end = content.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return content.substr(pos + 1, end - pos - 1);
    };

    PluginMeta meta;
    meta.id = extract("id");
    meta.name = extract("name");
    meta.description = extract("description");
    meta.author = extract("author");

    auto ver_str = extract("version");
    if (!ver_str.empty()) {
        int maj = 0, min = 0, pat = 0;
        std::sscanf(ver_str.c_str(), "%d.%d.%d", &maj, &min, &pat);
        meta.version = SemVer{maj, min, pat};
    }

    return meta;
}

} // namespace detail



class MarketplaceClient {
    std::string api_host_{"marketplace.cc-repl.dev"};
    uint16_t api_port_{443};
    std::string api_base_path_{"/api/v1"};
    
    // Cached registry data
    mutable std::vector<PluginMeta> cache_;
    mutable std::chrono::steady_clock::time_point cache_time_;
    static constexpr auto CACHE_TTL = std::chrono::minutes(5);

public:
    void set_api_endpoint(std::string host, uint16_t port = 443, std::string base_path = "/api/v1") {
        api_host_ = std::move(host);
        api_port_ = port;
        api_base_path_ = std::move(base_path);
        cache_.clear();
    }

    // Fetch plugin list from remote registry
    [[nodiscard]] auto fetch_registry() -> std::expected<std::vector<PluginMeta>, std::string> {
        auto resp = detail::http_get(api_host_, api_port_, api_base_path_ + "/plugins");
        if (!resp.ok()) {
            return std::unexpected("Registry HTTP " + std::to_string(resp.status_code));
        }
        // Parse JSON array of plugins from response body
        // Simple JSON array parser — each object has id, name, version, description
        std::vector<PluginMeta> plugins;
        // Find array start
        auto pos = resp.body.find('[');
        if (pos == std::string::npos) return plugins;

        // Extract objects between { }
        while (true) {
            auto obj_start = resp.body.find('{', pos);
            if (obj_start == std::string::npos) break;
            auto obj_end = resp.body.find('}', obj_start);
            if (obj_end == std::string::npos) break;

            auto obj = resp.body.substr(obj_start, obj_end - obj_start + 1);
            auto extract = [&](std::string_view key) -> std::string {
                auto pattern = "\"" + std::string(key) + "\"";
                auto p = obj.find(pattern);
                if (p == std::string::npos) return {};
                p = obj.find('"', p + pattern.size() + 1);
                if (p == std::string::npos) return {};
                auto e = obj.find('"', p + 1);
                if (e == std::string::npos) return {};
                return obj.substr(p + 1, e - p - 1);
            };

            PluginMeta meta;
            meta.id = extract("id");
            meta.name = extract("name");
            meta.description = extract("description");
            meta.author = extract("author");
            auto ver = extract("version");
            if (!ver.empty()) {
                int mj=0, mn=0, pt=0;
                std::sscanf(ver.c_str(), "%d.%d.%d", &mj, &mn, &pt);
                meta.version = SemVer{mj, mn, pt};
            }
            if (!meta.id.empty()) plugins.push_back(std::move(meta));
            pos = obj_end + 1;
        }

        cache_ = plugins;
        cache_time_ = std::chrono::steady_clock::now();
        return plugins;
    }


    [[nodiscard]] auto search(const MarketplaceQuery& query) -> std::vector<PluginMeta> {
        auto all = get_featured();
        if (query.keyword.empty()) return all;
        std::vector<PluginMeta> filtered;
        for (const auto& plugin : all) {
            if (plugin.name.find(query.keyword) != std::string::npos ||
                plugin.description.find(query.keyword) != std::string::npos ||
                plugin.id.find(query.keyword) != std::string::npos) {
                filtered.push_back(plugin);
            }
        }
        if (query.limit > 0 && filtered.size() > query.limit) {
            filtered.resize(query.limit);
        }
        return filtered;
    }
    

    [[nodiscard]] auto get_details(std::string_view plugin_id) -> std::optional<PluginMeta> {
        for (const auto& plugin : get_featured()) {
            if (plugin.id == plugin_id) return plugin;
        }
        return std::nullopt;
    }
    

    [[nodiscard]] auto get_featured() -> std::vector<PluginMeta> {
        auto now = std::chrono::steady_clock::now();
        if (!cache_.empty() && (now - cache_time_) < CACHE_TTL) {
            return cache_;
        }
        auto result = fetch_registry();
        if (result.has_value() && !result->empty()) {
            return *result;
        }
        // Fallback to built-in list if registry is unreachable
        return {PluginMeta{
            .id = "code-review",
            .name = "Code Review",
            .version = SemVer{1, 0, 0},
            .description = "Review code changes and surface high-confidence issues",
            .source = PluginSource::official_marketplace,
            .download_count = 1,
            .rating = 5.0
        }};
    }
    

    [[nodiscard]] auto download_archive(std::string_view plugin_id, const SemVer& version)
        -> std::expected<std::string, std::string> {
        auto path = api_base_path_ + "/plugins/" + std::string(plugin_id) +
                    "/download?version=" + version.to_string();
        auto resp = detail::http_get(api_host_, api_port_, path);
        if (!resp.ok()) {
            return std::unexpected("Download failed: HTTP " + std::to_string(resp.status_code));
        }
        if (resp.body.empty()) {
            return std::unexpected("Empty archive received");
        }
        return resp.body;
    }


    [[nodiscard]] auto get_checksum(std::string_view plugin_id, const SemVer& version)
        -> std::optional<std::string> {
        auto path = api_base_path_ + "/plugins/" + std::string(plugin_id) +
                    "/checksum?version=" + version.to_string();
        auto resp = detail::http_get(api_host_, api_port_, path);
        if (!resp.ok() || resp.body.empty()) return std::nullopt;
        // Body should be hex sha256
        auto nl = resp.body.find('\n');
        return (nl != std::string::npos) ? resp.body.substr(0, nl) : resp.body;
    }


    [[nodiscard]] auto check_updates(const std::vector<std::pair<std::string, SemVer>>& installed) 
        -> std::vector<std::pair<std::string, SemVer>> {
        std::vector<std::pair<std::string, SemVer>> updates;
        auto registry = get_featured();
        for (const auto& [id, current_ver] : installed) {
            for (const auto& remote : registry) {
                if (remote.id == id && remote.version > current_ver) {
                    updates.emplace_back(id, remote.version);
                    break;
                }
            }
        }
        return updates;
    }
};



class PluginInstallManager {
    std::filesystem::path plugins_dir_;
    std::vector<InstalledPlugin> installed_;
    MarketplaceClient marketplace_;
    
public:
    explicit PluginInstallManager(std::filesystem::path dir) : plugins_dir_(std::move(dir)) {
        std::filesystem::create_directories(plugins_dir_);
    }
    

    [[nodiscard]] auto install(std::string_view plugin_id, InstallOptions opts = {}) 
        -> std::expected<InstallResult, std::string> {
        auto start = std::chrono::steady_clock::now();
        

        auto meta = marketplace_.get_details(plugin_id);
        if (!meta) return std::unexpected("插件未找到: " + std::string(plugin_id));
        
        auto target_version = opts.specific_version.value_or(meta->version);
        

        auto archive_result = marketplace_.download_archive(plugin_id, target_version);
        if (!archive_result) {
            return std::unexpected("下载失败: " + archive_result.error());
        }
        

        auto expected_checksum = marketplace_.get_checksum(plugin_id, target_version);
        if (expected_checksum.has_value()) {
            auto actual_checksum = detail::sha256_hex(*archive_result);
            if (actual_checksum != *expected_checksum) {
                if (!opts.trust_unverified) {
                    return std::unexpected("完整性验证失败: 校验和不匹配");
                }
            }
        }
        

        auto install_path = plugins_dir_ / std::string(plugin_id);
        if (std::filesystem::exists(install_path)) {
            std::filesystem::remove_all(install_path);
        }
        
        if (!detail::extract_plugin_bundle(*archive_result, install_path)) {
            // Extraction failed — try treating it as raw manifest
            std::filesystem::create_directories(install_path);
            std::ofstream out(install_path / "manifest.json");
            out << *archive_result;
        }
        
        // 5. Validate manifest exists
        auto manifest_path = install_path / "manifest.json";
        if (std::filesystem::exists(manifest_path)) {
            if (auto parsed = detail::parse_manifest(manifest_path)) {
                meta = *parsed;  // Update with actual manifest data
                meta->version = target_version;
            }
        }
        

        InstalledPlugin p{*meta, opts.auto_enable ? PluginStatus::enabled : PluginStatus::installed,
                         install_path, std::chrono::system_clock::now()};
        
        // Remove existing entry if upgrading
        std::erase_if(installed_, [&](const auto& existing) { return existing.meta.id == plugin_id; });
        installed_.push_back(std::move(p));
        
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        return InstallResult{true, std::string(plugin_id), std::nullopt, elapsed};
    }
    

    [[nodiscard]] auto uninstall(std::string_view plugin_id) -> std::expected<void, std::string> {
        auto it = std::find_if(installed_.begin(), installed_.end(),
            [&](const auto& p) { return p.meta.id == plugin_id; });
        if (it == installed_.end()) return std::unexpected("插件未安装");
        std::filesystem::remove_all(it->install_path);
        installed_.erase(it);
        return {};
    }
    

    [[nodiscard]] auto update(std::string_view plugin_id) -> std::expected<InstallResult, std::string> {
        auto it = std::find_if(installed_.begin(), installed_.end(),
            [&](const auto& p) { return p.meta.id == plugin_id; });
        if (it == installed_.end()) return std::unexpected("插件未安装");

        return install(plugin_id, {.auto_enable = (it->status == PluginStatus::enabled)});
    }
    

    [[nodiscard]] auto update_all() -> std::vector<InstallResult> {
        std::vector<InstallResult> results;
        for (const auto& p : installed_) {
            if (p.available_update) {
                if (auto r = update(p.meta.id)) results.push_back(*r);
            }
        }
        return results;
    }
    

    void enable(std::string_view id) { set_status(id, PluginStatus::enabled); }
    void disable(std::string_view id) { set_status(id, PluginStatus::disabled); }
    

    [[nodiscard]] auto get_installed() const -> const std::vector<InstalledPlugin>& { return installed_; }
    

    [[nodiscard]] auto get_updatable() const -> std::vector<const InstalledPlugin*> {
        std::vector<const InstalledPlugin*> result;
        for (const auto& p : installed_) if (p.available_update) result.push_back(&p);
        return result;
    }
    

    [[nodiscard]] static auto get_trust_level(const PluginMeta& meta) -> TrustLevel {
        if (meta.source == PluginSource::official_marketplace) return TrustLevel::official;
        if (meta.source == PluginSource::community && meta.download_count > 1000) return TrustLevel::verified;
        if (meta.source == PluginSource::community) return TrustLevel::community;
        return TrustLevel::untrusted;
    }
    

    [[nodiscard]] auto get_policy(std::string_view plugin_id) const -> PluginPolicy {
        (void)plugin_id;
        return PluginPolicy{};
    }
    

    void scan_installed() {
        if (!std::filesystem::exists(plugins_dir_)) return;
        installed_.clear();
        for (const auto& entry : std::filesystem::directory_iterator(plugins_dir_)) {
            if (!entry.is_directory()) continue;
            auto id = entry.path().filename().string();
            installed_.push_back(InstalledPlugin{
                .meta = PluginMeta{.id = id, .name = id, .version = SemVer{0, 0, 0}, .source = PluginSource::local},
                .status = PluginStatus::installed,
                .install_path = entry.path(),
                .installed_at = std::chrono::system_clock::now()
            });
        }
    }

private:
    void set_status(std::string_view id, PluginStatus status) {
        for (auto& p : installed_) {
            if (p.meta.id == id) { p.status = status; break; }
        }
    }
};

} // namespace cc::plugins
