/// @file concrete_migrations.cppm
/// @brief Public interface for concrete configuration migrations.
module;

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

export module cc.migrations.concrete;

import cc.utils.json;

namespace cc::migrations::concrete {

using cc::utils::json::JsonDoc;
using cc::utils::json::JsonMutDoc;
using cc::utils::json::JsonMutVal;
using cc::utils::json::JsonVal;

/// Read-only view into the three input config trees.
export struct DetectCtx {
    JsonVal global;
    JsonVal user;
    JsonVal local;
};

/// Mutable view of the three logical config trees inside one document.
export struct ConfigCtx {
    std::unique_ptr<JsonMutDoc> doc;
    JsonMutVal global;
    JsonMutVal user;
    JsonMutVal local;
};

/// Concrete migration descriptor with split detect/apply phases.
export struct MigrationEntry {
    std::string id;
    std::string description;
    int version;
    std::function<bool(const DetectCtx&)> detect;
    std::function<void(ConfigCtx&)> apply;
};

export [[nodiscard]] auto get_all_migrations()
    -> const std::vector<MigrationEntry>&;

export [[nodiscard]] std::vector<const MigrationEntry*> detect_pending(
    const DetectCtx& ctx);

export [[nodiscard]] std::vector<std::string> run_all_migrations(
    const JsonDoc* global_src,
    const JsonDoc* user_src,
    const JsonDoc* local_src);

}  // namespace cc::migrations::concrete
