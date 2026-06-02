# CC-REPL 架构审查报告与 C++ 迁移接口定义

> 日期: 2026-05-28  
> 审查范围: 原 TypeScript 项目架构、C++ 迁移项目架构、核心模块接口

---

## 目录

1. [原项目架构分析](#原项目架构分析)
2. [C++ 迁移项目架构评估](#c-迁移项目架构评估)
3. [C++20 模块接口标准](#c20-模块接口标准)
4. [代码审查与质量标准](#代码审查与质量标准)
5. [模块依赖关系分析](#模块依赖关系分析)
6. [核心模块接口文档](#核心模块接口文档)
7. [迁移建议与注意事项](#迁移建议与注意事项)

---

## 原项目架构分析

### 1.1 项目概览

原项目是一个基于 TypeScript/React 的命令行交互式 AI 编程助手，主要功能包括：

- 与 Claude 模型进行对话
- 内置工具集（文件操作、代码执行、搜索等）
- MCP (Model Context Protocol) 服务器集成
- IDE 桥接功能
- 会话管理与持久化
- 多 Agent 协调

### 1.2 核心模块结构

```
src/
├── main.tsx              # 应用入口，命令行解析
├── QueryEngine.ts        # 核心查询引擎
├── query.ts              # 查询执行逻辑
├── Tool.ts               # 工具定义与注册表
├── commands.ts           # 命令系统
├── context.ts            # 上下文管理
├── history.ts            # 历史记录
├── bootstrap/            # 启动与状态管理
├── bridge/               # IDE 桥接
├── commands/             # 内置命令实现
├── components/           # UI 组件
├── hooks/                # React Hooks
├── ink/                  # 终端 UI 引擎
├── services/             # 服务层
│   ├── api/              # API 客户端
│   ├── mcp/              # MCP 协议
│   ├── lsp/              # LSP 客户端
│   ├── oauth/            # OAuth 认证
│   ├── analytics/        # 分析与遥测
│   └── compact/          # 对话压缩
├── state/                # 状态管理
├── tools/                # 工具实现
├── utils/                # 工具函数
└── plugins/              # 插件系统
```

### 1.3 QueryEngine.ts 核心设计

**职责**: 管理查询生命周期和会话状态

**核心类**:

```typescript
class QueryEngine {
    private config: QueryEngineConfig
    private mutableMessages: Message[]
    private abortController: AbortController
    private permissionDenials: SDKPermissionDenial[]
    private totalUsage: NonNullableUsage
    private readFileState: FileStateCache
    
    async* submitMessage(prompt: string | ContentBlockParam[]): AsyncGenerator<SDKMessage>
    interrupt(): void
    getMessages(): readonly Message[]
}
```

**配置项**:
- `tools`: 可用工具集合
- `commands`: 内置命令
- `mcpClients`: MCP 客户端
- `canUseTool`: 权限检查回调
- `getAppState/setAppState`: 状态管理
- `readFileCache`: 文件读取缓存

**工作流**:
1. 处理用户输入（斜杠命令、权限验证）
2. 构建系统提示词
3. 调用 `query()` 执行模型请求
4. 管理工具调用与结果
5. 持久化会话状态

### 1.4 main.tsx 入口设计

**职责**: 引导应用启动，解析命令行参数，初始化子系统

**关键流程**:
1. 解析 CLI 参数（`--print`, `--model`, `--resume` 等）
2. 初始化日志、信号处理、事件循环
3. 加载配置
4. 初始化各子系统（API 客户端、工具注册表、MCP、技能、Hooks）
5. 分派到无头模式或交互模式

### 1.5 原架构优点

✅ 清晰的分层架构（UI → Hooks → Services → Utils）  
✅ 模块化设计，职责分离  
✅ 功能丰富的工具生态系统  
✅ 良好的状态管理  
✅ 支持多种运行模式（交互式、SDK、远程）  

### 1.6 原架构挑战

⚠️ TypeScript 运行时开销  
⚠️ 启动时间较长  
⚠️ 内存占用较高  
⚠️ 包管理复杂  
⚠️ 模块间耦合度高  

---

## C++ 迁移项目架构评估

### 2.1 项目结构概览

```
cpp_migration/
├── CMakeLists.txt        # 主构建配置
├── src/
│   ├── main.cpp          # 应用入口
│   ├── CMakeLists.txt    # 模块库定义
│   ├── core/             # 核心引擎
│   ├── tools/            # 工具系统
│   ├── commands/         # 命令系统
│   ├── services/         # 服务层
│   ├── ui/               # 终端 UI
│   ├── state/            # 状态管理
│   ├── hooks/            # Hooks 系统
│   ├── bridge/           # IDE 桥接
│   ├── skills/           # 技能系统
│   ├── plugins/          # 插件系统
│   ├── utils/            # 工具模块
│   └── benchmarks/       # 基准测试
└── docs/                 # 文档
```

### 2.2 模块划分与 CMake 目标

迁移项目将系统划分为多个 C++23 模块库：

| CMake 目标 | 职责 | 依赖 |
|-----------|------|------|
| `cc_utils` | 基础工具（无内部依赖） | `yyjson`, `uv_a`, `httplib` |
| `cc_state` | 状态管理 | `cc_utils`, `yyjson` |
| `cc_core` | 核心引擎（类型、工具、查询、会话、Swarm） | `cc_utils`, `cc_state`, `CURL`, `yyjson`, `uv_a` |
| `cc_tools` | 工具注册表与实现 | `cc_core`, `cc_utils`, `cc_state` |
| `cc_commands` | 命令注册表与实现 | `cc_core`, `cc_tools`, `cc_state`, `cc_utils` |
| `cc_services` | API、MCP、LSP、分析等服务 | `cc_core`, `cc_utils`, `cc_state`, `CURL`, `yyjson`, `uv_a`, `httplib` |
| `cc_ui` | 终端 UI（FTXUI 基础） | `cc_core`, `cc_state`, `cc_utils`, `ftxui` |
| `cc_screens` | 屏幕系统 | `cc_ui`, `cc_core`, `cc_state`, `cc_services`, `ftxui` |
| `cc_bridge` | IDE 桥接 | `cc_core`, `cc_state`, `cc_utils`, `yyjson`, `uv_a` |
| `cc_skills` | 技能系统 | `cc_core`, `cc_tools`, `cc_state`, `cc_utils` |
| `cc_hooks` | Hooks 系统 | `cc_core`, `cc_tools`, `cc_state`, `cc_utils`, `cc_services`, `cc_skills`, `cc_ui` |
| `cc_app` | 主执行程序 | 所有模块 |

### 2.3 迁移架构优点

✅ C++23 模块支持，编译隔离好  
✅ 清晰的依赖层次，自底向上设计  
✅ 使用现代库（libuv、yyjson、FTXUI、httplib）  
✅ 模块化构建，可独立测试  
✅ 目标是性能优化（启动时间、内存、执行速度）  

### 2.4 迁移架构关注点

⚠️ 模块数量多，需谨慎管理依赖  
⚠️ 部分模块采用 `.cppm` + 骨架实现，需要完善  
⚠️ UI 引擎重写复杂度高（从自定义 Ink 迁移到 FTXUI）  
⚠️ 异步模型差异（Node.js 事件循环 vs libuv）  

---

## C++20 模块接口标准

### 3.1 模块命名规范

#### 3.1.1 模块名格式

```cpp
// 主模块格式: <project>.<layer>.<component>
export module cc.core.query_engine;
export module cc.tools.bash_tool;
export module cc.services.api.client;

// 分区模块格式（如适用）
export module cc.core:types;
export module cc.core:query_engine;
```

#### 3.1.2 命名空间与模块对应

模块名直接映射到命名空间层次：

```cpp
// 模块: cc.core.query_engine
namespace cc::core {
    export class QueryEngine { /* ... */ };
}
```

### 3.2 模块导出约定

#### 3.2.1 导出宏（可选）

```cpp
// 在公共头中定义（如需要）
#define CC_EXPORT export

// 使用
CC_EXPORT class QueryEngine { };
```

#### 3.2.2 导出层级

1. **公共 API 导出**（面向应用）
   ```cpp
   export module cc.core.query_engine;
   
   import <memory>;
   import <string>;
   import <expected>;
   import cc.core.types;
   import cc.core.config;
   
   export namespace cc::core {
       class QueryEngine { /* ... */ };
       using QueryResult = std::expected<Response, Error>;
   }
   ```

2. **内部实现不导出**
   ```cpp
   module; // 全局模块片段
   #include <internal_header.h>
   
   export module cc.core.query_engine;
   
   // 内部辅助函数不导出
   namespace cc::core::detail {
       auto helper() -> void { /* ... */ }
   }
   
   export namespace cc::core {
       // 公共 API
       class QueryEngine { /* ... */ };
   }
   ```

### 3.3 接口设计原则

#### 3.3.1 类型安全

```cpp
// ✅ 使用强类型而非原始类型
export struct SessionId {
    std::string value;
    explicit SessionId(std::string v) : value(std::move(v)) {}
};

// ✅ 使用 expected<T, E> 替代异常（性能关键路径）
export auto query(std::string_view prompt) 
    -> std::expected<Response, QueryError>;
```

#### 3.3.2 资源管理

```cpp
// ✅ 使用 RAII，避免手动生命周期管理
export class QueryEngine {
public:
    explicit QueryEngine(Config config);
    ~QueryEngine();
    
    // 禁止复制
    QueryEngine(const QueryEngine&) = delete;
    QueryEngine& operator=(const QueryEngine&) = delete;
    
    // 允许移动
    QueryEngine(QueryEngine&&) noexcept;
    QueryEngine& operator=(QueryEngine&&) noexcept;
};
```

#### 3.3.3 异步接口

```cpp
// ✅ 使用 callback/future 或协程（C++20）
export module cc.core.query_engine;

import <future>;
import <functional>;

export namespace cc::core {
    // 回调风格
    using ResponseCallback = std::function<void(std::expected<Response, Error>)>;
    void query_async(std::string_view prompt, ResponseCallback cb);
    
    // Future 风格
    auto query_async(std::string_view prompt) 
        -> std::future<std::expected<Response, Error>>;
    
    // C++20 协程风格（推荐）
    auto query_co(std::string_view prompt) 
        -> cppcoro::task<std::expected<Response, Error>>;
}
```

### 3.4 模块文件结构

每个功能模块应包含：

```
src/core/
├── types.cppm            # 类型定义
├── tool.cppm             # 工具基类
├── command.cppm          # 命令基类
├── config.cppm           # 配置
├── session.cppm          # 会话管理
├── query_engine.cppm     # 查询引擎（重点）
├── coordinator.cppm      # 协调器
└── ...
```

---

## 代码审查与质量标准

### 4.1 代码审查清单

#### 4.1.1 架构与设计

- [ ] 模块职责清晰，符合单一职责原则
- [ ] 依赖关系符合分层架构（无反向依赖）
- [ ] 接口设计考虑了扩展性
- [ ] 状态管理线程安全（如适用）

#### 4.1.2 性能考量

- [ ] 避免不必要的拷贝，使用 `std::move`
- [ ] 关键路径使用 `std::string_view` 而非 `std::string`
- [ ] 考虑内存布局与缓存友好性
- [ ] 异步操作正确使用 libuv

#### 4.1.3 安全性

- [ ] 输入验证完善
- [ ] 权限检查在正确位置
- [ ] 资源泄露检查（文件句柄、内存、网络连接）
- [ ] 无缓冲区溢出风险

#### 4.1.4 可维护性

- [ ] 命名清晰、一致
- [ ] 复杂逻辑有注释说明
- [ ] 避免过度模板化
- [ ] 错误信息明确、可操作

### 4.2 编码规范

#### 4.2.1 命名约定

```cpp
// 类型: PascalCase
export class QueryEngine { };
export struct Message { };
enum class PermissionMode { };

// 函数/方法: snake_case
export auto submit_message(std::string_view prompt) -> Result;

// 变量: snake_case
std::string session_id;
int max_turns;

// 私有成员: trailing underscore
class QueryEngine {
    Config config_;
    std::vector<Message> messages_;
};

// 常量: kCamelCase 或 UPPER_SNAKE_CASE
constexpr int kMaxRetries = 5;
constexpr std::string_view kDefaultModel = "claude-3-opus";
```

#### 4.2.2 格式化

使用 `.clang-format` 统一格式化：

```yaml
BasedOnStyle: Google
ColumnLimit: 100
IndentWidth: 4
UseTab: Never
PointerAlignment: Left
AccessModifierOffset: -4
```

### 4.3 测试要求

- [ ] 单元测试覆盖率 ≥ 80%
- [ ] 关键业务逻辑有集成测试
- [ ] 使用 GoogleTest 框架
- [ ] 测试可重复、无外部依赖（优先）

### 4.4 文档要求

- [ ] 公共 API 有 Doxygen 注释
- [ ] 复杂算法有说明文档
- [ ] 示例代码展示基本用法
- [ ] 更新 ARCHITECTURE_REVIEW.md 当架构变化

---

## 模块依赖关系分析

### 5.1 依赖层次图

```
┌─────────────────────────────────────────────────────────────┐
│                        cc_app (main)                        │
└────────────┬────────────────────────────────────────────────┘
             │
    ┌────────┼────────┬────────┬────────┬────────┬────────┐
    │        │        │        │        │        │        │
    ▼        ▼        ▼        ▼        ▼        ▼        ▼
┌────────┐ ┌──────┐ ┌───────┐ ┌──────┐ ┌───────┐ ┌───────┐ ┌──────┐
│cc_hooks│ │cc_ui │ │cc_srv │ │cc_cmd│ │cc_tools│ │cc_brid│ │cc_sk │
└────┬───┘ └───┬──┘ └───┬───┘ └───┬──┘ └───┬───┘ └───┬───┘ └──┬───┘
     │         │         │          │         │          │         │
     └─────────┴─────────┴──────────┼─────────┴──────────┴─────────┘
                                     │
                              ┌──────▼───────┐
                              │   cc_core   │
                              └──────┬───────┘
                                     │
                              ┌──────▼───────┐
                              │   cc_state  │
                              └──────┬───────┘
                                     │
                              ┌──────▼───────┐
                              │   cc_utils  │
                              └──────────────┘
```

### 5.2 依赖规则

1. **自底向上**: 高层依赖低层，低层不依赖高层
2. **无循环**: 模块间无循环依赖
3. **最小依赖**: 仅依赖需要的模块
4. **抽象依赖**: 依赖接口而非实现（如适用）

### 5.3 核心模块依赖分析

#### QueryEngine 依赖

```
cc.core.query_engine
├── cc.core.types
├── cc.core.config
├── cc.core.session
├── cc.core.tool
├── cc.utils.error
├── cc.utils.json
└── [外部: libuv, yyjson]
```

---

## 核心模块接口文档

### 6.1 cc.core.query_engine（重点）

#### 模块概述
对应原项目 `QueryEngine.ts`，负责查询生命周期管理、会话状态、工具调用协调。

#### 导出接口

```cpp
export module cc.core.query_engine;

import <memory>;
import <string>;
import <vector>;
import <expected>;
import <functional>;
import <coroutine>;
import cc.core.types;
import cc.core.config;
import cc.core.session;
import cc.core.tool;
import cc.services.api.client;
import cc.hooks.permissions;
import cc.utils.error;

export namespace cc::core {

// 前向声明
class ToolRegistry;
class PermissionChecker;

/// 查询引擎配置
struct QueryEngineConfig {
    Config config;
    ToolRegistry* tool_registry;
    PermissionChecker* permission_checker;
    uv_loop_t* event_loop;
    std::optional<std::string> custom_system_prompt;
    std::optional<int> max_turns;
    std::optional<double> max_budget_usd;
};

/// 查询结果
struct QueryResult {
    std::string response;
    Usage usage;
    std::vector<Message> conversation;
    std::vector<PermissionDenial> denials;
};

/// 查询引擎（核心类）
class QueryEngine {
public:
    /// 构造函数
    explicit QueryEngine(QueryEngineConfig config);
    
    /// 析构函数
    ~QueryEngine();
    
    // 禁止复制
    QueryEngine(const QueryEngine&) = delete;
    QueryEngine& operator=(const QueryEngine&) = delete;
    
    // 允许移动
    QueryEngine(QueryEngine&&) noexcept;
    QueryEngine& operator=(QueryEngine&&) noexcept;

    /// 同步提交消息
    auto submit_message(std::string_view prompt)
        -> std::expected<QueryResult, QueryError>;
    
    /// 异步提交消息（回调风格）
    using ResultCallback = std::function<void(std::expected<QueryResult, QueryError>)>;
    void submit_message_async(std::string_view prompt, ResultCallback callback);
    
    /// 异步提交消息（协程风格）
    auto submit_message_co(std::string_view prompt)
        -> cppcoro::task<std::expected<QueryResult, QueryError>>;
    
    /// 中断当前查询
    void interrupt();
    
    /// 获取当前会话消息
    auto get_messages() const -> const std::vector<Message>&;
    
    /// 获取会话 ID
    auto get_session_id() const -> SessionId;
    
    /// 设置模型
    void set_model(std::string_view model);
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cc::core
```

#### 使用示例

```cpp
import cc.core.query_engine;
import cc.core.config;
import cc.tools.registry;
import cc.hooks.permissions;

using namespace cc::core;

int main() {
    auto config = Config::load();
    auto tool_reg = ToolRegistry{};
    auto perm_check = PermissionChecker{PermissionMode::default_mode};
    
    auto qe_config = QueryEngineConfig{
        .config = config,
        .tool_registry = &tool_reg,
        .permission_checker = &perm_check,
        .event_loop = uv_default_loop()
    };
    
    auto engine = QueryEngine{qe_config};
    
    auto result = engine.submit_message("Hello, Claude!");
    if (result.has_value()) {
        std::println("{}", result->response);
    }
    
    return 0;
}
```

### 6.2 cc.core.config

#### 模块概述
管理应用配置，加载/保存 `~/.claude/config.json`。

#### 导出接口

```cpp
export module cc.core.config;

import <string>;
import <optional>;
import <expected>;
import <filesystem>;
import cc.utils.error;
import cc.utils.json;

export namespace cc::core {

struct Config {
    // API 配置
    std::string api_key;
    std::string api_url = "https://api.anthropic.com";
    std::string default_model = "claude-3-opus-20240229";
    
    // 会话配置
    std::filesystem::path config_dir;
    std::filesystem::path session_dir;
    
    // MCP 配置
    std::vector<std::filesystem::path> mcp_server_configs;
    
    // 加载配置文件
    static auto load(const std::filesystem::path& path)
        -> std::expected<Config, ConfigError>;
    
    // 保存配置文件
    auto save(const std::filesystem::path& path) const
        -> std::expected<void, ConfigError>;
    
    // 获取默认配置
    static auto default_config() -> Config;
    
    // 获取默认配置目录
    static auto default_config_dir() -> std::filesystem::path;
};

} // namespace cc::core
```

### 6.3 cc.tools.registry

#### 模块概述
工具注册表，管理所有可用工具（内置、MCP、插件）。

#### 导出接口

```cpp
export module cc.tools.registry;

import <memory>;
import <string>;
import <vector>;
import <unordered_map>;
import <expected>;
import cc.core.tool;
import cc.utils.error;

export namespace cc::tools {

class ToolRegistry {
public:
    ToolRegistry() = default;
    
    /// 注册工具
    void register_tool(std::unique_ptr<Tool> tool);
    
    /// 按名称获取工具
    auto get_tool(std::string_view name) 
        -> std::expected<Tool*, ToolError>;
    
    /// 获取所有工具
    auto get_all_tools() const -> std::vector<Tool*>;
    
    /// 工具是否存在
    auto has_tool(std::string_view name) const -> bool;
    
private:
    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

} // namespace cc::tools
```

### 6.4 cc.ui.terminal（重点）

#### 模块概述
对应原项目 `src/ink/`，负责终端 UI 渲染、事件处理。基于 FTXUI 实现。

#### 导出接口

```cpp
export module cc.ui.terminal;

import <memory>;
import <string>;
import <functional>;
import cc.core.query_engine;
import cc.tools.registry;
import cc.commands.registry;
import cc.hooks.context;
import cc.hooks.permissions;

export namespace cc::ui {

class TerminalUI {
public:
    /// 创建终端 UI
    static auto create(uv_loop_t* loop) 
        -> std::unique_ptr<TerminalUI>;
    
    ~TerminalUI();
    
    /// 设置依赖
    void set_query_engine(core::QueryEngine* engine);
    void set_tool_registry(tools::ToolRegistry* registry);
    void set_command_registry(commands::CommandRegistry* registry);
    void set_context_manager(hooks::ContextManager* ctx);
    void set_permission_checker(hooks::PermissionChecker* checker);
    
    /// 恢复会话
    void restore_session(core::Session session);
    
    /// 提交提示词
    void submit_prompt(std::string_view prompt);
    
    /// 运行事件循环
    void run(std::atomic<bool>& shutdown_requested);
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cc::ui
```

### 6.5 cc.services.api.client

#### 模块概述
对应原项目 `src/services/api/`，提供 Anthropic API 客户端。

#### 导出接口

```cpp
export module cc.services.api.client;

import <string>;
import <vector>;
import <memory>;
import <expected>;
import <functional>;
import cc.core.types;
import cc.core.config;
import cc.utils.error;

export namespace cc::services::api {

struct ApiConfig {
    std::string api_key;
    std::string api_url;
    std::string model;
    int max_tokens = 4096;
    float temperature = 0.7f;
};

struct StreamChunk {
    std::optional<std::string> text_delta;
    std::optional<std::vector<ToolUse>> tool_calls;
    std::optional<Usage> usage;
    bool done = false;
};

using StreamCallback = std::function<void(StreamChunk)>;

class ApiClient {
public:
    static auto create(ApiConfig config) 
        -> std::unique_ptr<ApiClient>;
    
    virtual ~ApiClient() = default;
    
    /// 同步非流式请求
    virtual auto complete(const std::vector<Message>& messages)
        -> std::expected<Response, ApiError> = 0;
    
    /// 异步流式请求
    virtual void complete_stream(
        const std::vector<Message>& messages,
        StreamCallback callback
    ) = 0;
};

} // namespace cc::services::api
```

### 6.6 cc.hooks.permissions

#### 模块概述
权限管理，检查工具是否可以执行。

#### 导出接口

```cpp
export module cc.hooks.permissions;

import <string>;
import <functional>;
import cc.core.tool;

export namespace cc::hooks {

enum class PermissionMode {
    ask,           // 每次询问
    auto_approve,  // 自动批准
    auto_deny,     // 自动拒绝
    plan,          // 先计划再执行
};

struct PermissionRequest {
    std::string tool_name;
    std::string tool_description;
    std::string input;
};

struct PermissionDecision {
    bool allowed;
    std::string reason;
    bool remember = false;
};

class PermissionChecker {
public:
    explicit PermissionChecker(PermissionMode mode);
    
    /// 检查权限
    auto check_permission(const PermissionRequest& req)
        -> PermissionDecision;
    
    /// 获取当前模式
    auto get_mode() const -> PermissionMode;
    
    /// 设置模式
    void set_mode(PermissionMode mode);
    
private:
    PermissionMode mode_;
};

} // namespace cc::hooks
```

---

## 迁移建议与注意事项

### 7.1 核心模块迁移优先级

| 优先级 | 模块 | 说明 |
|-------|------|------|
| **P0** | `cc_core` (query_engine, session, tool) | 核心引擎，必须先完成 |
| **P0** | `cc_utils` (json, file, async, process) | 基础工具，所有模块依赖 |
| **P0** | `cc_services/api` | API 调用，核心功能 |
| **P1** | `cc_tools` (核心工具: bash, file_*, grep, glob) | 基础工具集 |
| **P1** | `cc_state` | 状态管理 |
| **P1** | `cc_ui` (terminal, basic components) | 交互界面 |
| **P2** | `cc_commands` | 斜杠命令 |
| **P2** | `cc_services/mcp` | MCP 集成 |
| **P3+** | 其余模块 | 按需求逐步迁移 |

### 7.2 关键技术注意事项

#### 7.2.1 异步模型适配

原项目使用 Node.js 事件循环，迁移到 C++ 使用 libuv：
- 保持回调风格 API 便于迁移
- 逐步引入 C++20 协程
- 使用 `uv_async_t` 进行线程间通信

#### 7.2.2 内存管理

- 核心对象（QueryEngine、Session）使用 `unique_ptr` + Pimpl
- 消息列表使用 `std::vector<Message>`，避免频繁分配
- 大对象（如长对话）考虑内存映射或分段

#### 7.2.3 JSON 处理

使用 `yyjson` 作为 JSON 库，因为：
- 性能优秀
- API 简单
- 头文件仅，易于集成

#### 7.2.4 UI 迁移策略

原项目 `src/ink/` 是自定义 UI 引擎，迁移方案：
1. **方案 A**（推荐）: 基于 FTXUI 重写 UI
   - 优点: 成熟库，社区活跃
   - 缺点: 需要适配设计
   
2. **方案 B**: 迁移自定义 Ink 引擎到 C++
   - 优点: 保持原行为
   - 缺点: 工作量大

当前 CMake 配置选择了方案 A。

### 7.3 验证策略

1. **功能对等**: 确保核心功能与原项目一致
2. **性能基准**: 使用 `pare-benchmark` 比较性能
3. **集成测试**: 端到端测试关键路径
4. **渐进替换**: 可考虑用 C++ 模块逐步替换原项目（通过 FFI）

---

## 附录

### A. 原项目核心文件索引

| 原文件 | C++ 对应模块 | 状态 |
|--------|-------------|------|
| `src/QueryEngine.ts` | `cc.core.query_engine` | ⚠️ 骨架实现 |
| `src/main.tsx` | `main.cpp` | ✅ 已实现 |
| `src/query.ts` | `cc.services.api.client` | ⚠️ 骨架实现 |
| `src/Tool.ts` | `cc.core.tool` + `cc.tools.registry` | ⚠️ 骨架实现 |
| `src/commands.ts` | `cc.core.command` + `cc.commands.registry` | ⚠️ 骨架实现 |
| `src/context.ts` | `cc.hooks.context` | ⚠️ 骨架实现 |
| `src/history.ts` | `cc.core.session` | ⚠️ 骨架实现 |
| `src/ink/` | `cc_ui` (FTXUI 基础) | ⚠️ 部分实现 |
| `src/services/api/` | `cc.services.api` | ⚠️ 骨架实现 |
| `src/services/mcp/` | `cc.services.mcp` | ⚠️ 骨架实现 |
| `src/tools/` | `cc.tools/` | ⚠️ 部分实现 |

### B. 参考资料

- [C++20 模块规范](https://en.cppreference.com/w/cpp/language/modules)
- [libuv 文档](http://docs.libuv.org/)
- [FTXUI 仓库](https://github.com/ArthurSonzogni/FTXUI)
- [yyjson 仓库](https://github.com/ibireme/yyjson)
- [原项目 README](../CLAUDE.md)
- [迁移计划](../docs/plan/MIGRATION_PLAN.md)

---

**文档版本**: 1.0  
**最后更新**: 2026-05-28  
**维护者**: 架构团队
