module;
#include <optional>
#include <string>

export module cc.hooks.main_loop_model;

export namespace cc::hooks {

enum class ModelType {
  claude_3_5_sonnet,
  claude_3_opus,
  claude_3_haiku,
  custom
};

struct ModelConfig {
  std::string name;
  ModelType type = ModelType::claude_3_5_sonnet;
  std::string api_endpoint;
  size_t context_window = 200000;
};

class MainLoopModelHook {
public:
  MainLoopModelHook() = default;

  auto set_model(ModelConfig config) -> void {
    current_model_ = std::move(config);
  }

  auto get_model() const -> const ModelConfig& {
    return current_model_;
  }

  auto use_default_model() -> void {
    current_model_ = ModelConfig{
      .name = "claude-3-5-sonnet",
      .type = ModelType::claude_3_5_sonnet,
      .api_endpoint = {},
      .context_window = 200000
    };
  }

  auto set_session_model(std::optional<ModelConfig> model) -> void {
    session_model_ = std::move(model);
  }

  auto get_effective_model() const -> const ModelConfig& {
    if (session_model_) {
      return *session_model_;
    }
    return current_model_;
  }

private:
  ModelConfig current_model_{
    .name = "claude-3-5-sonnet",
    .type = ModelType::claude_3_5_sonnet,
    .api_endpoint = {},
    .context_window = 200000
  };
  std::optional<ModelConfig> session_model_;
};

}
