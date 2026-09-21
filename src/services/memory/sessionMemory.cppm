// Session Memory Service Module
module;
#include <chrono>
#include <expected>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

export module cc.services.memory.sessionMemory;

import cc.utils.error;

export namespace cc::services::memory {

using cc::utils::Result;

// Memory item
struct MemoryItem {
    std::string id;
    std::string content;
    std::string type;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    int importance = 0;
};

// Session memory service
class SessionMemoryService {
public:
    SessionMemoryService();
    
    // Add a memory item
    Result<void> add_memory(const MemoryItem& item);
    
    // Get a memory item by ID
    Result<MemoryItem> get_memory(const std::string& id);
    
    // Get all memories
    Result<std::vector<MemoryItem>> get_all_memories();
    
    // Search memories by content
    Result<std::vector<MemoryItem>> search_memories(const std::string& query);
    
    // Delete a memory
    Result<void> delete_memory(const std::string& id);
    
    // Clear all memories
    Result<void> clear_all();
    
private:
    std::unordered_map<std::string, MemoryItem> memories_;
    std::mutex mutex_;
};

// Constructor
SessionMemoryService::SessionMemoryService() {}

// Add memory
Result<void> SessionMemoryService::add_memory(const MemoryItem& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    memories_[item.id] = item;
    return {};
}

// Get memory
Result<MemoryItem> SessionMemoryService::get_memory(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = memories_.find(id);
    if (it == memories_.end()) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::not_found, "memory item not found"));
    }
    return it->second;
}

// Get all memories
Result<std::vector<MemoryItem>> SessionMemoryService::get_all_memories() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MemoryItem> result;
    result.reserve(memories_.size());
    for (const auto& [id, item] : memories_) {
        result.push_back(item);
    }
    return result;
}

// Search memories
Result<std::vector<MemoryItem>> SessionMemoryService::search_memories(const std::string& query) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MemoryItem> result;
    
    for (const auto& [id, item] : memories_) {
        if (item.content.find(query) != std::string::npos) {
            result.push_back(item);
        }
    }
    
    return result;
}

// Delete memory
Result<void> SessionMemoryService::delete_memory(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    memories_.erase(id);
    return {};
}

// Clear all
Result<void> SessionMemoryService::clear_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    memories_.clear();
    return {};
}

} // namespace cc::services::memory
