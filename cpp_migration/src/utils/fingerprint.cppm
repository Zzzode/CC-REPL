module;

#include <string>
#include <string_view>
#include <array>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <random>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
// For Linux, use a simple SHA-256 implementation or openssl
#include <openssl/sha.h>
#endif

export module cc.utils.fingerprint;

export namespace cc::utils {

// Compute SHA-256 hash of a string, returning hex digest
inline std::string hash_string(std::string_view input) {
    std::array<unsigned char, 32> hash{};

#ifdef __APPLE__
    CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), hash.data());
#else
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash.data());
#endif

    // Convert to hex string
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (auto byte : hash) {
        hex << std::setw(2) << static_cast<int>(byte);
    }
    return hex.str();
}

// Get a stable machine identifier (persists across reboots)
inline std::string get_machine_id() {
#ifdef __APPLE__
    // macOS: use IOPlatformUUID via ioreg
    FILE* pipe = popen("ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | "
                       "awk '/IOPlatformUUID/{print $3}' | tr -d '\"'", "r");
    if (pipe) {
        std::array<char, 128> buffer{};
        std::string result;
        if (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result = buffer.data();
        }
        pclose(pipe);

        // Trim whitespace
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
            result.pop_back();

        if (!result.empty()) {
            return hash_string(result);
        }
    }
#else
    // Linux: read /etc/machine-id
    std::ifstream file("/etc/machine-id");
    if (file.is_open()) {
        std::string machine_id;
        std::getline(file, machine_id);
        if (!machine_id.empty()) {
            return hash_string(machine_id);
        }
    }

    // Fallback: /var/lib/dbus/machine-id
    std::ifstream dbus_file("/var/lib/dbus/machine-id");
    if (dbus_file.is_open()) {
        std::string machine_id;
        std::getline(dbus_file, machine_id);
        if (!machine_id.empty()) {
            return hash_string(machine_id);
        }
    }
#endif

    // Final fallback: hash hostname + boot time
    char hostname[256]{};
    gethostname(hostname, sizeof(hostname));
    return hash_string(std::string("fallback:") + hostname);
}

// Get a session-unique fingerprint (changes each process invocation)
inline std::string get_session_fingerprint() {
    // Combine PID, start time, and random seed for uniqueness
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    pid_t pid = getpid();

    // Generate some randomness
    std::mt19937_64 rng(now ^ (static_cast<uint64_t>(pid) << 32));
    uint64_t random_val = rng();

    std::ostringstream identity;
    identity << "session:" << pid << ":" << now << ":" << random_val;

    return hash_string(identity.str());
}

} // namespace cc::utils
