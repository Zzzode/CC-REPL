module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module core.screens;


export namespace screens {

// Forward declarations
struct Command;
struct Tool;
struct Message;
struct ThinkingConfig {
    bool enabled;
    int budgetTokens;
};

// Doctor screen related types
struct DiagnosticInfo {
    std::string installationType;
    std::string version;
    std::optional<std::string> packageManager;
    std::optional<std::string> installationPath;
    std::optional<std::string> invokedBinary;
    std::optional<std::string> configInstallMethod;
    struct {
        bool working;
        std::string mode;
        std::optional<std::string> systemPath;
    } ripgrepStatus;
    std::optional<std::string> recommendation;
    std::vector<std::string> multipleInstallations;
    std::vector<std::string> warnings;
    bool autoUpdates;
    std::optional<bool> hasUpdatePermissions;
};

struct DoctorProps {
    std::function<void(std::optional<std::string>, std::optional<std::map<std::string, std::string>>)> onDone;
};

// REPL screen related types
enum class Screen {
    Prompt,
    Transcript
};

enum class SpinnerMode {
    Requesting,
    Responding,
    ToolUse
};

struct StreamingToolUse {
    std::string toolName;
    std::string input;
    bool isStreaming;
    std::chrono::system_clock::time_point startedAt;
};

struct StreamingThinking {
    std::string content;
    bool isStreaming;
    std::optional<std::chrono::system_clock::time_point> streamingEndedAt;
};

struct REPLProps {
    std::vector<Command> commands;
    bool debug;
    std::vector<Tool> initialTools;
    std::optional<std::vector<Message>> initialMessages;
    std::optional<std::function<std::vector<Message>()>> pendingHookMessages;
    std::optional<std::vector<std::string>> initialFileHistorySnapshots;
    std::optional<std::vector<std::string>> initialContentReplacements;
    std::optional<std::string> initialAgentName;
    std::optional<std::string> initialAgentColor;
    std::optional<std::vector<std::string>> mcpClients;
    std::optional<std::map<std::string, std::string>> dynamicMcpConfig;
    bool autoConnectIdeFlag;
    bool strictMcpConfig;
    std::optional<std::string> systemPrompt;
    std::optional<std::string> appendSystemPrompt;
    std::optional<std::function<bool(std::string, std::vector<Message>)>> onBeforeQuery;
    std::optional<std::function<void(std::vector<Message>)>> onTurnComplete;
    bool disabled;
    std::optional<std::string> mainThreadAgentDefinition;
    bool disableSlashCommands;
    std::optional<std::string> taskListId;
    std::optional<std::string> remoteSessionConfig;
    std::optional<std::string> directConnectConfig;
    std::optional<std::string> sshSession;
    ThinkingConfig thinkingConfig;
};

// ResumeConversation related types
struct LogOption {
    std::string id;
    std::string fullPath;
    std::string displayName;
    std::chrono::system_clock::time_point timestamp;
    std::optional<int> prNumber;
    bool isSidechain;
    int value;
};

struct ResumeConversationProps {
    std::vector<Command> commands;
    std::vector<std::string> worktreePaths;
    std::vector<Tool> initialTools;
    std::optional<std::vector<std::string>> mcpClients;
    std::optional<std::map<std::string, std::string>> dynamicMcpConfig;
    bool debug;
    std::optional<std::string> mainThreadAgentDefinition;
    bool autoConnectIdeFlag;
    bool strictMcpConfig;
    std::optional<std::string> systemPrompt;
    std::optional<std::string> appendSystemPrompt;
    std::optional<std::string> initialSearchQuery;
    bool disableSlashCommands;
    bool forkSession;
    std::optional<std::string> taskListId;
    std::optional<std::variant<bool, int, std::string>> filterByPr;
    ThinkingConfig thinkingConfig;
    std::optional<std::function<void(std::vector<Message>)>> onTurnComplete;
};

struct ResumeData {
    std::vector<Message> messages;
    std::optional<std::vector<std::string>> fileHistorySnapshots;
    std::optional<std::vector<std::string>> contentReplacements;
    std::optional<std::string> agentName;
    std::optional<std::string> agentColor;
    std::optional<std::string> mainThreadAgentDefinition;
};

// Supporting types for completeness
struct Command {
    std::string name;
    std::string description;
    std::vector<std::string> aliases;
    bool isEnabled;
};

struct Tool {
    std::string name;
    std::string description;
    std::string version;
    bool isBuiltin;
};

struct Message {
    std::string id;
    std::string role;
    std::string content;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> uuid;
};

// Helper functions
inline std::optional<int> parsePrIdentifier(const std::string& value) {
    auto parse_positive_int = [](const std::string& digits) -> std::optional<int> {
        try {
            int num = std::stoi(digits);
            if (num > 0) {
                return num;
            }
        } catch (...) {}
        return std::nullopt;
    };

    std::smatch match;

    static const std::regex direct_number(R"(^\s*([1-9]\d*)\s*$)", std::regex::optimize);
    if (std::regex_match(value, match, direct_number)) {
        return parse_positive_int(match[1].str());
    }

    static const std::regex github_pull_url(
        R"(^\s*(?:https?://)?github\.com/[^/\s]+/[^/\s]+/pull/([1-9]\d*)(?:[/#?].*)?\s*$)",
        std::regex::icase | std::regex::optimize);
    if (std::regex_match(value, match, github_pull_url)) {
        return parse_positive_int(match[1].str());
    }

    return std::nullopt;
}

// Screen state management
class DoctorScreen {
public:
    explicit DoctorScreen(DoctorProps props) : props_(std::move(props)) {}
    
    void setDiagnostic(const DiagnosticInfo& diag) {
        diagnostic_ = diag;
    }
    
    const std::optional<DiagnosticInfo>& getDiagnostic() const {
        return diagnostic_;
    }
    
    void dismiss() {
        if (props_.onDone) {
            props_.onDone("Claude Code diagnostics dismissed", std::nullopt);
        }
    }
    
private:
    DoctorProps props_;
    std::optional<DiagnosticInfo> diagnostic_;
};

class REPLScreen {
public:
    explicit REPLScreen(REPLProps props) : props_(std::move(props)) {}
    
    void setScreen(Screen screen) {
        screen_ = screen;
    }
    
    Screen getScreen() const {
        return screen_;
    }
    
    void setStreamMode(SpinnerMode mode) {
        streamMode_ = mode;
    }
    
    SpinnerMode getStreamMode() const {
        return streamMode_;
    }
    
    void setShowAllInTranscript(bool show) {
        showAllInTranscript_ = show;
    }
    
    bool getShowAllInTranscript() const {
        return showAllInTranscript_;
    }
    
    void addStreamingToolUse(const StreamingToolUse& toolUse) {
        streamingToolUses_.push_back(toolUse);
    }
    
    void clearStreamingToolUses() {
        streamingToolUses_.clear();
    }
    
    void setStreamingThinking(const std::optional<StreamingThinking>& thinking) {
        streamingThinking_ = thinking;
    }
    
    const std::optional<StreamingThinking>& getStreamingThinking() const {
        return streamingThinking_;
    }
    
    void setAbortController(std::shared_ptr<void> controller) {
        abortController_ = std::move(controller);
    }
    
    const std::shared_ptr<void>& getAbortController() const {
        return abortController_;
    }
    
private:
    REPLProps props_;
    Screen screen_ = Screen::Prompt;
    SpinnerMode streamMode_ = SpinnerMode::Responding;
    bool showAllInTranscript_ = false;
    std::string editorStatus_;
    std::vector<StreamingToolUse> streamingToolUses_;
    std::optional<StreamingThinking> streamingThinking_;
    std::shared_ptr<void> abortController_;
};

class ResumeConversationScreen {
public:
    explicit ResumeConversationScreen(ResumeConversationProps props) : props_(std::move(props)) {}
    
    void setLogs(const std::vector<LogOption>& logs) {
        logs_ = logs;
        logCount_ = static_cast<int>(logs.size());
    }
    
    const std::vector<LogOption>& getLogs() const {
        return logs_;
    }
    
    void setLoading(bool loading) {
        loading_ = loading;
    }
    
    bool isLoading() const {
        return loading_;
    }
    
    void setResuming(bool resuming) {
        resuming_ = resuming;
    }
    
    bool isResuming() const {
        return resuming_;
    }
    
    void setShowAllProjects(bool show) {
        showAllProjects_ = show;
    }
    
    bool getShowAllProjects() const {
        return showAllProjects_;
    }
    
    void setResumeData(const ResumeData& data) {
        resumeData_ = data;
    }
    
    const std::optional<ResumeData>& getResumeData() const {
        return resumeData_;
    }
    
    void setCrossProjectCommand(const std::optional<std::string>& cmd) {
        crossProjectCommand_ = cmd;
    }
    
    const std::optional<std::string>& getCrossProjectCommand() const {
        return crossProjectCommand_;
    }
    
    void loadMoreLogs(int count) {
        // Implementation would load more logs
        if (loadMoreLogsCallback_) {
            loadMoreLogsCallback_(count);
        }
    }
    
    void setLoadMoreLogsCallback(std::function<void(int)> callback) {
        loadMoreLogsCallback_ = std::move(callback);
    }
    
    void cancel() {
        // In real implementation, this would exit the process
    }
    
    void selectLog([[maybe_unused]] const LogOption& log) {
        // Implementation would handle log selection
    }
    
private:
    ResumeConversationProps props_;
    std::vector<LogOption> logs_;
    int logCount_ = 0;
    bool loading_ = true;
    bool resuming_ = false;
    bool showAllProjects_ = false;
    std::optional<ResumeData> resumeData_;
    std::optional<std::string> crossProjectCommand_;
    std::function<void(int)> loadMoreLogsCallback_;
};

// Factory functions
inline std::unique_ptr<DoctorScreen> createDoctorScreen(DoctorProps props) {
    return std::make_unique<DoctorScreen>(std::move(props));
}

inline std::unique_ptr<REPLScreen> createREPLScreen(REPLProps props) {
    return std::make_unique<REPLScreen>(std::move(props));
}

inline std::unique_ptr<ResumeConversationScreen> createResumeConversationScreen(ResumeConversationProps props) {
    return std::make_unique<ResumeConversationScreen>(std::move(props));
}

} // namespace screens
