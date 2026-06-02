// C++23 Utils Index Module
// Aggregates all utility modules
module;

export module cc.utils;

// Core utilities
export import cc.utils.error;
export import cc.utils.async;
export import cc.utils.string;

// I/O and filesystem
export import cc.utils.file;
export import cc.utils.path;
export import cc.utils.platform;
export import cc.utils.env;

// Git and version control
export import cc.utils.git;
export import cc.utils.github_utils;

// Bash and command parsing
export import cc.utils.bash_parser;

// Caching and performance
export import cc.utils.cache;
export import cc.utils.circular_buffer;

// Logging and diagnostics
export import cc.utils.log;

// Networking
export import cc.utils.http;

// Crypto
export import cc.utils.crypto;

// JSON
export import cc.utils.json;

// Process management
export import cc.utils.process;

// Agent and model
export import cc.utils.agent_model;
export import cc.utils.model_aliases;

// Memory and session
export import cc.utils.memory;
export import cc.utils.memdir;
export import cc.utils.session;
export import cc.utils.session_storage;

// UI and terminal
export import cc.utils.terminal;
export import cc.utils.input_router;

// Code analysis
export import cc.utils.code_indexing;

// Other utilities
export import cc.utils.pdf;
export import cc.utils.permissions;
export import cc.utils.sandbox;
export import cc.utils.query_helpers;
export import cc.utils.task_output;
export import cc.utils.suggestions;
export import cc.utils.swarm;
export import cc.utils.native_utils;
