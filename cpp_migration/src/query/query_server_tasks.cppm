module;

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

export module core.query_server_tasks;


// Query System
export namespace query {

// Query config
struct QueryConfig {
    std::string model;
    int maxOutputTokens;
    double temperature;
    double topP;
    int topK;
    std::optional<std::string> systemPrompt;
    bool enableThinking;
    int thinkingBudgetTokens;
};

inline QueryConfig getDefaultQueryConfig() {
    return {
        "claude-3-5-sonnet",
        8192,
        0.7,
        0.9,
        40,
        std::nullopt,
        false,
        0
    };
}

// Dependencies tracking
struct QueryDependencies {
    std::vector<std::string> filesRead;
    std::vector<std::string> toolsUsed;
    std::vector<std::string> commandsExecuted;
};

// Stop hooks
using StopHook = std::function<bool()>;

class StopHookManager {
public:
    void addHook(StopHook hook) {
        hooks_.push_back(std::move(hook));
    }
    
    bool shouldStop() const {
        for (const auto& hook : hooks_) {
            if (hook()) {
                return true;
            }
        }
        return false;
    }
    
    void clear() {
        hooks_.clear();
    }
    
private:
    std::vector<StopHook> hooks_;
};

// Token budget
struct TokenBudget {
    int totalBudget;
    int usedTokens;
    int remainingTokens;
    
    void reset(int budget) {
        totalBudget = budget;
        usedTokens = 0;
        remainingTokens = budget;
    }
    
    void consume(int tokens) {
        usedTokens += tokens;
        remainingTokens = std::max(0, totalBudget - usedTokens);
    }
    
    bool hasBudget(int tokens = 1) const {
        return remainingTokens >= tokens;
    }
};

// Query result
struct QueryResult {
    std::string content;
    int inputTokens;
    int outputTokens;
    std::chrono::milliseconds duration;
    bool success;
    std::optional<std::string> error;
    QueryDependencies dependencies;
};

// Query engine interface
class QueryEngine {
public:
    explicit QueryEngine(QueryConfig config = getDefaultQueryConfig())
        : config_(std::move(config)) {}
    
    void setConfig(const QueryConfig& config) {
        config_ = config;
    }
    
    const QueryConfig& getConfig() const {
        return config_;
    }
    
    TokenBudget& getTokenBudget() {
        return tokenBudget_;
    }
    
    StopHookManager& getStopHooks() {
        return stopHooks_;
    }
    
    void addDependencies(const QueryDependencies& deps) {
        dependencies_.filesRead.insert(dependencies_.filesRead.end(),
                                         deps.filesRead.begin(), deps.filesRead.end());
        dependencies_.toolsUsed.insert(dependencies_.toolsUsed.end(),
                                       deps.toolsUsed.begin(), deps.toolsUsed.end());
        dependencies_.commandsExecuted.insert(dependencies_.commandsExecuted.end(),
                                               deps.commandsExecuted.begin(), deps.commandsExecuted.end());
    }
    
    const QueryDependencies& getDependencies() const {
        return dependencies_;
    }
    
    QueryResult execute(const std::string& prompt) {
        auto start = std::chrono::steady_clock::now();
        
        QueryResult result;
        if (prompt.empty()) {
            result.success = false;
            result.error = "Prompt cannot be empty";
            result.content.clear();
            result.inputTokens = 0;
            result.outputTokens = 0;
            result.duration = std::chrono::milliseconds{0};
            return result;
        }

        if (stopHooks_.shouldStop()) {
            result.success = false;
            result.error = "Query stopped by hook";
            result.content.clear();
            result.inputTokens = static_cast<int>(prompt.size()) / 4;
            result.outputTokens = 0;
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            result.dependencies = dependencies_;
            return result;
        }

        result.success = true;
        result.content = config_.systemPrompt.value_or("Query processed") + ": " + prompt;
        result.inputTokens = static_cast<int>(prompt.size()) / 4;
        result.outputTokens = result.content.size() / 4;
        tokenBudget_.consume(result.inputTokens + result.outputTokens);
        
        auto end = std::chrono::steady_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        result.dependencies = dependencies_;
        
        return result;
    }
    
private:
    QueryConfig config_;
    TokenBudget tokenBudget_;
    StopHookManager stopHooks_;
    QueryDependencies dependencies_;
};

} // namespace query

// Server System
export namespace server {

// Direct connect types
struct DirectConnectConfig {
    std::string serverUrl;
    std::string apiKey;
    int timeoutMs;
    bool useTls;
};

struct ConnectionState {
    bool connected;
    std::optional<std::string> sessionId;
    std::optional<std::chrono::system_clock::time_point> connectedAt;
    std::optional<std::string> lastError;
};

// Direct connect manager
class DirectConnectManager {
public:
    explicit DirectConnectManager(DirectConnectConfig config)
        : config_(std::move(config)) {}
    
    bool connect() {
        state_.connected = true;
        state_.sessionId = "session_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        state_.connectedAt = std::chrono::system_clock::now();
        state_.lastError = std::nullopt;
        return true;
    }
    
    void disconnect() {
        state_.connected = false;
        state_.sessionId = std::nullopt;
    }
    
    const ConnectionState& getState() const {
        return state_;
    }
    
    bool isConnected() const {
        return state_.connected;
    }
    
    void setConfig(const DirectConnectConfig& config) {
        config_ = config;
    }
    
private:
    DirectConnectConfig config_;
    ConnectionState state_;
};

// Server types
enum class ServerType {
    Local,
    Remote,
    SelfHosted
};

struct ServerInfo {
    ServerType type;
    std::string version;
    std::vector<std::string> capabilities;
    std::map<std::string, std::string> metadata;
};

} // namespace server

// Tasks System
export namespace tasks {

// Task pill label
struct PillLabel {
    std::string text;
    std::string color;
    bool isBold;
};

inline PillLabel createPillLabel(const std::string& text, const std::string& color = "default", bool bold = false) {
    return {text, color, bold};
}

// Stop task
struct StopTaskRequest {
    std::string taskId;
    std::optional<std::string> reason;
    bool force;
};

// Task types
enum class TaskStatus {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled
};

enum class TaskPriority {
    Low,
    Normal,
    High,
    Urgent
};

struct Task {
    std::string id;
    std::string name;
    std::string description;
    TaskStatus status;
    TaskPriority priority;
    std::chrono::system_clock::time_point createdAt;
    std::optional<std::chrono::system_clock::time_point> startedAt;
    std::optional<std::chrono::system_clock::time_point> completedAt;
    std::optional<std::string> error;
    std::vector<std::string> dependencies;
    std::map<std::string, std::string> metadata;
};

// Task manager
class TaskManager {
public:
    TaskManager() = default;
    
    std::string createTask(const std::string& name, const std::string& description,
                           TaskPriority priority = TaskPriority::Normal) {
        std::string id = "task_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        Task task;
        task.id = id;
        task.name = name;
        task.description = description;
        task.status = TaskStatus::Pending;
        task.priority = priority;
        task.createdAt = std::chrono::system_clock::now();
        tasks_[id] = std::move(task);
        return id;
    }
    
    void startTask(const std::string& taskId) {
        if (tasks_.contains(taskId)) {
            tasks_[taskId].status = TaskStatus::Running;
            tasks_[taskId].startedAt = std::chrono::system_clock::now();
        }
    }
    
    void completeTask(const std::string& taskId) {
        if (tasks_.contains(taskId)) {
            tasks_[taskId].status = TaskStatus::Completed;
            tasks_[taskId].completedAt = std::chrono::system_clock::now();
        }
    }
    
    void failTask(const std::string& taskId, const std::string& error) {
        if (tasks_.contains(taskId)) {
            tasks_[taskId].status = TaskStatus::Failed;
            tasks_[taskId].error = error;
            tasks_[taskId].completedAt = std::chrono::system_clock::now();
        }
    }
    
    void cancelTask(const std::string& taskId, const std::optional<std::string>& reason = std::nullopt) {
        if (tasks_.contains(taskId)) {
            tasks_[taskId].status = TaskStatus::Cancelled;
            if (reason) {
                tasks_[taskId].error = *reason;
            }
            tasks_[taskId].completedAt = std::chrono::system_clock::now();
        }
    }
    
    std::optional<Task> getTask(const std::string& taskId) const {
        if (tasks_.contains(taskId)) {
            return tasks_.at(taskId);
        }
        return std::nullopt;
    }
    
    std::vector<Task> getAllTasks() const {
        std::vector<Task> result;
        for (const auto& [id, task] : tasks_) {
            result.push_back(task);
        }
        return result;
    }
    
    std::vector<Task> getTasksByStatus(TaskStatus status) const {
        std::vector<Task> result;
        for (const auto& [id, task] : tasks_) {
            if (task.status == status) {
                result.push_back(task);
            }
        }
        return result;
    }
    
    void clearCompleted() {
        std::erase_if(tasks_, [](const auto& pair) {
            return pair.second.status == TaskStatus::Completed ||
                   pair.second.status == TaskStatus::Cancelled;
        });
    }
    
private:
    std::map<std::string, Task> tasks_;
};

} // namespace tasks

// Bootstrap, Daemon, Runners
export namespace cc_system {

// Bootstrap state
struct BootstrapState {
    bool initialized;
    std::string originalCwd;
    std::string projectRoot;
    std::string claudeConfigHome;
    std::map<std::string, std::string> envVars;
    std::optional<std::string> sessionId;
};

// Daemon
struct DaemonConfig {
    int port;
    std::string socketPath;
    bool autoStart;
    int maxIdleTimeoutMs;
};

class Daemon {
public:
    explicit Daemon(DaemonConfig config) : config_(std::move(config)) {}
    
    bool start() {
        running_ = true;
        startTime_ = std::chrono::system_clock::now();
        return true;
    }
    
    void stop() {
        running_ = false;
    }
    
    bool isRunning() const {
        return running_;
    }
    
private:
    DaemonConfig config_;
    bool running_ = false;
    std::optional<std::chrono::system_clock::time_point> startTime_;
};

// Environment runner
struct EnvironmentConfig {
    std::string workingDir;
    std::map<std::string, std::string> env;
    std::vector<std::string> allowedCommands;
    bool networkEnabled;
};

class EnvironmentRunner {
public:
    explicit EnvironmentRunner(EnvironmentConfig config) : config_(std::move(config)) {}
    
    std::string executeCommand(const std::string& cmd, const std::vector<std::string>& args) {
        return "Command executed: " + cmd;
    }
    
private:
    EnvironmentConfig config_;
};

// Self-hosted runner
struct SelfHostedConfig {
    std::string modelPath;
    int gpuDevice;
    int numWorkers;
    std::string cacheDir;
};

class SelfHostedRunner {
public:
    explicit SelfHostedRunner(SelfHostedConfig config) : config_(std::move(config)) {}
    
    bool loadModel() {
        return true;
    }
    
    std::string generate(const std::string& prompt) {
        return "Self-hosted response";
    }
    
private:
    SelfHostedConfig config_;
    bool modelLoaded_ = false;
};

// Upstream proxy
struct ProxyConfig {
    std::string upstreamUrl;
    std::optional<std::string> proxyUrl;
    int timeoutMs;
    std::map<std::string, std::string> headers;
};

class UpstreamProxy {
public:
    explicit UpstreamProxy(ProxyConfig config) : config_(std::move(config)) {}
    
    std::string forwardRequest(const std::string& path, const std::string& body) {
        return "Proxy response";
    }
    
private:
    ProxyConfig config_;
};

} // namespace system
