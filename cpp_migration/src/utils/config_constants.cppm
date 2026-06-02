module;
#include <string_view>

export module cc.utils.config_constants;

export namespace cc::utils {

// File names
inline constexpr std::string_view CONFIG_FILE_NAME = "config.json";
inline constexpr std::string_view SETTINGS_FILE_NAME = "settings.json";
inline constexpr std::string_view CREDENTIALS_FILE = "credentials.json";
inline constexpr std::string_view CLAUDE_MD_FILE = "CLAUDE.md";
inline constexpr std::string_view SESSIONS_DIR = "sessions";
inline constexpr std::string_view LOGS_DIR = "logs";

// Config keys - API
inline constexpr std::string_view KEY_API_KEY = "api_key";
inline constexpr std::string_view KEY_API_BASE_URL = "api_base_url";
inline constexpr std::string_view KEY_MODEL = "model";
inline constexpr std::string_view KEY_MAX_TOKENS = "max_tokens";
inline constexpr std::string_view KEY_TEMPERATURE = "temperature";

// Config keys - Behavior
inline constexpr std::string_view KEY_STREAM = "stream";
inline constexpr std::string_view KEY_THINKING = "thinking";
inline constexpr std::string_view KEY_VERBOSE = "verbose";
inline constexpr std::string_view KEY_DEBUG = "debug";
inline constexpr std::string_view KEY_FAST_MODE = "fast_mode";

// Config keys - UI
inline constexpr std::string_view KEY_THEME = "theme";
inline constexpr std::string_view KEY_COLOR = "color";
inline constexpr std::string_view KEY_WIDTH = "width";
inline constexpr std::string_view KEY_UNICODE = "unicode";

// Config keys - Session
inline constexpr std::string_view KEY_SESSION_TIMEOUT = "session_timeout";
inline constexpr std::string_view KEY_MAX_SESSIONS = "max_sessions";
inline constexpr std::string_view KEY_AUTO_COMPACT = "auto_compact";

// Config keys - Provider
inline constexpr std::string_view KEY_PROVIDER = "provider";
inline constexpr std::string_view KEY_BEDROCK_REGION = "bedrock_region";
inline constexpr std::string_view KEY_BEDROCK_PROFILE = "bedrock_profile";
inline constexpr std::string_view KEY_VERTEX_PROJECT = "vertex_project";
inline constexpr std::string_view KEY_VERTEX_REGION = "vertex_region";

// Environment variable prefixes
inline constexpr std::string_view ENV_PREFIX_CLAUDE = "CLAUDE_";
inline constexpr std::string_view ENV_PREFIX_ANTHROPIC = "ANTHROPIC_";

// Limits
inline constexpr int MAX_CONFIG_FILE_SIZE = 1024 * 1024; // 1MB
inline constexpr int DEFAULT_SESSION_TIMEOUT_SECS = 3600;
inline constexpr int DEFAULT_MAX_SESSIONS = 10;

} // namespace cc::utils
