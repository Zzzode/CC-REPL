/// @file test_state.cpp


/// persistence, on_change_app_state, ftxui_integration

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <memory>

#include <gtest/gtest.h>

import cc.state.app_state;
import cc.state.store;
import cc.state.selectors;
import cc.state.persistence;
import cc.state.on_change;
import cc.state.ftxui_integration;
import cc.session.history;
import cc.types.types;
import cc.cli.update;
import cc.services.mcp.auth;
import cc.services.mcp.types;
import cc.utils.error;
import cc.constants.prompts;

namespace {

[[nodiscard]] std::unique_ptr<cc::state::AppStore> make_test_store() {
    return std::make_unique<cc::state::AppStore>(
        cc::state::get_default_app_state(),
        &cc::state::app_reducer);
}

[[nodiscard]] std::shared_ptr<cc::state::AppStore> make_shared_test_store() {
    return std::make_shared<cc::state::AppStore>(
        cc::state::get_default_app_state(),
        &cc::state::app_reducer);
}

} // namespace
// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(AppState, DefaultStateIsValid) {
    auto state = cc::state::get_default_app_state();
    EXPECT_LE(state.created_at, std::chrono::system_clock::now());
    EXPECT_FALSE(state.verbose);
    EXPECT_FALSE(state.is_loading);
    EXPECT_FALSE(state.is_streaming);
}

TEST(AppState, ObservableState) {
    cc::state::ObservableState obs_state;
    int change_count = 0;
    
    auto sub_id = obs_state.subscribe([&change_count](const auto&, const auto&) {
        change_count++;
    });
    EXPECT_NE(sub_id, 0u);
    
    auto new_state = cc::state::get_default_app_state();
    new_state.verbose = true;
    obs_state.set(new_state);
    
    EXPECT_EQ(change_count, 1);
    EXPECT_TRUE(obs_state.get().verbose);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(StateStore, InitialState) {
    auto store = make_test_store();
    auto state = store->get_state();

    EXPECT_LE(state.created_at, std::chrono::system_clock::now());
}

TEST(StateStore, DispatchSetVerbose) {
    auto store = make_test_store();

    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});

    auto state = store->get_state();
    EXPECT_TRUE(state.verbose);
}

TEST(StateStore, DispatchSetLoading) {
    auto store = make_test_store();

    store->dispatch(cc::state::Action{cc::state::ActionType::SetLoading, true});

    auto state = store->get_state();
    EXPECT_TRUE(state.is_loading);
}

TEST(StateStore, SubscribeReceivesNotifications) {
    auto store = make_test_store();
    int notify_count = 0;


    auto sub_id = store->subscribe([&notify_count](const auto& /*prev*/, const auto& /*next*/) {
        notify_count++;
    });

    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    store->dispatch(cc::state::Action{cc::state::ActionType::SetLoading, true});

    EXPECT_EQ(notify_count, 2);


    store->unsubscribe(sub_id);
    store->dispatch(cc::state::Action{cc::state::ActionType::SetStreaming, true});
    EXPECT_EQ(notify_count, 2);
}

TEST(StateStore, UnknownActionNoOp) {
    auto store = make_test_store();
    auto before = store->get_state();


    store->dispatch(cc::state::Action{cc::state::ActionType::EnableTool});
    auto after = store->get_state();

    EXPECT_EQ(before.verbose, after.verbose);
    EXPECT_EQ(before.is_loading, after.is_loading);
}

TEST(StateStore, MiddlewareSupport) {
    auto store = make_test_store();
    int middleware_count = 0;
    
    store->add_middleware([&middleware_count](cc::state::DispatchFn next) {
        return [next = std::move(next), &middleware_count](const cc::state::Action& action) {
            middleware_count++;
            next(action);
        };
    });
    
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    EXPECT_EQ(middleware_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(Selectors, IsVerbose) {
    auto state = cc::state::get_default_app_state();
    state.verbose = true;
    
    EXPECT_TRUE(cc::state::selectors::is_verbose(state));
}

TEST(Selectors, IsLoading) {
    auto state = cc::state::get_default_app_state();
    state.is_loading = true;
    
    EXPECT_TRUE(cc::state::selectors::is_loading(state));
}

TEST(Selectors, MemoizedSelector) {
    auto state = cc::state::get_default_app_state();
    int compute_count = 0;
    
    cc::state::selectors::MemoizedSelector<bool, bool> selector(
        [](const auto& s) {
            return s.verbose;
        },
        [&compute_count](const auto& s) {
            compute_count++;
            return s.verbose;
        }
    );
    

    auto result1 = selector.select(state);
    auto result2 = selector.select(state);
    EXPECT_EQ(result1, result2);
    EXPECT_EQ(compute_count, 1);
    

    state.verbose = true;
    auto result3 = selector.select(state);
    EXPECT_TRUE(result3);
    EXPECT_EQ(compute_count, 2);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(OnChangeAppState, StateChangeRegistry) {
    cc::state::on_change::StateChangeRegistry registry;
    int callback_count = 0;
    
    registry.register_callback([&callback_count](const auto&, const auto&) {
        callback_count++;
    });
    
    auto state1 = cc::state::get_default_app_state();
    auto state2 = cc::state::get_default_app_state();
    state2.verbose = true;
    
    registry.run_callbacks(state1, state2);
    EXPECT_EQ(callback_count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(FTXUIIntegration, VerboseIndicator) {
    cc::state::ftxui::VerboseIndicator indicator;
    
    auto store = make_shared_test_store();
    indicator.connect(store);
    
    EXPECT_EQ(indicator.get_text(), "");
    
    store->dispatch(cc::state::Action{cc::state::ActionType::SetVerbose, true});
    // Note: In real usage, the component would need to process the state change
    // This test verifies basic construction and API
    EXPECT_TRUE(indicator.get_last_state().verbose);
}

TEST(FTXUIIntegration, LoadingIndicator) {
    cc::state::ftxui::LoadingIndicator indicator;
    
    auto store = make_shared_test_store();
    indicator.connect(store);
    
    EXPECT_EQ(indicator.get_text(), "");
}

TEST(FTXUIIntegration, MessageCounter) {
    cc::state::ftxui::MessageCounter counter;
    
    auto store = make_shared_test_store();
    counter.connect(store);
    

    EXPECT_EQ(counter.get_last_state().messages.size(), 0);
}

TEST(FTXUIIntegration, ReactiveScreenManager) {
    auto store = make_shared_test_store();
    auto manager = cc::state::ftxui::make_reactive_screen_manager(store);
    
    auto indicator = std::make_shared<cc::state::ftxui::VerboseIndicator>();
    manager->add_component(indicator);
    
    EXPECT_EQ(manager->get_store(), store);
    manager->clear_components();
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(Persistence, StatePersistenceAPI) {

    auto state_file = std::filesystem::temp_directory_path() / "cc_repl_test_state.json";
    cc::state::persistence::StatePersistence persistence(state_file);
    
    auto state = cc::state::get_default_app_state();
    state.verbose = true;
    state.is_loading = false;
    

    auto save_result = persistence.save_state(state);
    ASSERT_TRUE(save_result.has_value());

    auto loaded = persistence.load_state();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->verbose);
    EXPECT_FALSE(loaded->is_loading);

    (void)persistence.delete_state();
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(SessionHistory, SaveAllPersistsCreatedConversationIds) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_save_test.json";
    std::filesystem::remove(storage_path);

    cc::core::ConversationStore store(storage_path.string());
    store.create_conversation();
    auto ids = store.get_conversation_ids();
    ASSERT_EQ(ids.size(), 1u);

    auto saved = store.save_all();
    ASSERT_TRUE(saved.has_value()) << saved.error().format();

    std::ifstream input(storage_path);
    ASSERT_TRUE(input.is_open());
    std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_NE(json.find(ids.front()), std::string::npos);
    EXPECT_NE(json.find("active_conversation_id"), std::string::npos);

    std::filesystem::remove(storage_path);
}

TEST(SessionHistory, LoadAllRestoresConversationIdsAndActiveSelection) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_load_test.json";
    std::filesystem::remove(storage_path);

    {
        cc::core::ConversationStore store(storage_path.string());
        store.create_conversation();
        auto ids = store.get_conversation_ids();
        ASSERT_EQ(ids.size(), 1u);
        ASSERT_TRUE(store.save_all().has_value());
    }

    cc::core::ConversationStore loaded(storage_path.string());
    auto result = loaded.load_all();
    ASSERT_TRUE(result.has_value()) << result.error().format();

    auto loaded_ids = loaded.get_conversation_ids();
    ASSERT_EQ(loaded_ids.size(), 1u);
    EXPECT_TRUE(loaded.switch_conversation(loaded_ids.front()));

    std::filesystem::remove(storage_path);
}

TEST(SessionHistory, LoadAllRestoresSavedMessages) {
    auto storage_path = std::filesystem::temp_directory_path() /
        "cc_repl_history_messages_test.json";
    std::filesystem::remove(storage_path);

    {
        cc::core::ConversationStore store(storage_path.string());
        auto* conversation = store.create_conversation();
        conversation->add_message(cc::core::UserMessage{
            cc::core::MessageBase{
                cc::core::MessageId{"msg_user_1"},
                std::chrono::system_clock::now(),
                {cc::core::TextBlock{"hello from persisted history"}}
            }
        });
        ASSERT_TRUE(store.save_all().has_value());
    }

    cc::core::ConversationStore loaded(storage_path.string());
    ASSERT_TRUE(loaded.load_all().has_value());
    auto* active = loaded.get_active_conversation();
    auto messages = active->get_messages();

    ASSERT_EQ(messages.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::core::UserMessage>(messages.front()));
    const auto& user = std::get<cc::core::UserMessage>(messages.front());
    ASSERT_EQ(user.content.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<cc::core::TextBlock>(user.content.front()));
    EXPECT_EQ(std::get<cc::core::TextBlock>(user.content.front()).text,
              "hello from persisted history");

    std::filesystem::remove(storage_path);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliUpdate, DownloadUpdateCopiesFileUrlPayload) {
    auto source_path = std::filesystem::temp_directory_path() /
        "cc_repl_update_source.bin";
    std::filesystem::remove(source_path);
    {
        std::ofstream output(source_path, std::ios::binary | std::ios::trunc);
        output << "real update payload";
    }

    auto downloaded = cc::cli::download_update("file://" + source_path.string());
    ASSERT_TRUE(downloaded.has_value()) << downloaded.error();
    ASSERT_TRUE(std::filesystem::exists(*downloaded));

    std::ifstream input(*downloaded, std::ios::binary);
    std::string payload((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(payload, "real update payload");

    std::filesystem::remove(source_path);
    std::filesystem::remove(*downloaded);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(McpAuth, FetchConfiguredMetadataFromFileUrl) {
    auto metadata_path = std::filesystem::temp_directory_path() /
        "cc_repl_mcp_oauth_metadata.json";
    std::filesystem::remove(metadata_path);
    {
        std::ofstream output(metadata_path, std::ios::trunc);
        output << R"({
            "authorization_endpoint":"https://auth.example.com/authorize",
            "token_endpoint":"https://auth.example.com/token",
            "scope":"openid profile"
        })";
    }

    auto metadata = cc::services::mcp::fetch_auth_server_metadata(
        "test-server",
        "https://mcp.example.com/sse",
        "file://" + metadata_path.string());

    ASSERT_TRUE(metadata.has_value()) << metadata.error().format();
    ASSERT_TRUE(metadata->has_value());
    EXPECT_EQ((*metadata)->authorization_endpoint, "https://auth.example.com/authorize");
    EXPECT_EQ((*metadata)->token_endpoint, "https://auth.example.com/token");
    ASSERT_TRUE((*metadata)->scope.has_value());
    EXPECT_EQ(*(*metadata)->scope, "openid profile");

    std::filesystem::remove(metadata_path);
}

TEST(McpAuth, XaaFlowDoesNotReturnUnimplementedError) {
    unsetenv("CLAUDE_CODE_ENABLE_XAA");

    cc::services::mcp::McpServerConfig server_config{
        .type = "http",
        .url = "https://mcp.example.com/mcp",
        .headers = {},
        .oauth = cc::services::mcp::McpOAuthConfig{
            .auth_server_metadata_url = std::nullopt,
            .callback_port = std::nullopt,
            .client_id = std::nullopt,
            .xaa = true}
    };

    auto result = cc::services::mcp::perform_mcp_oauth_flow(
        "test-server",
        server_config,
        [](const std::string&) {});

    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().code(), cc::utils::ErrorCode::unimplemented);
    EXPECT_NE(result.error().message().find("XAA is not enabled"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════════

TEST(SystemPrompts, ComputeSimpleEnvInfoIncludesDynamicRuntimeDetails) {
    auto env_info = cc::constants::prompts::compute_simple_env_info(
        "claude-sonnet-4-6",
        {"/tmp/cc-repl-extra"});

    EXPECT_NE(env_info.find("# Environment"), std::string::npos);
    EXPECT_NE(env_info.find("Primary working directory:"), std::string::npos);
    EXPECT_NE(env_info.find("Additional working directories:"), std::string::npos);
    EXPECT_NE(env_info.find("/tmp/cc-repl-extra"), std::string::npos);
    EXPECT_NE(env_info.find("Assistant knowledge cutoff is August 2025."), std::string::npos);
}

TEST(SystemPrompts, GetSystemPromptAssemblesStaticAndDynamicSections) {
    cc::constants::prompts::SystemPromptOptions options{
        .model = "claude-opus-4-6",
        .enabled_tools = {"Read", "Write"},
        .additional_working_directories = {},
        .simple = false,
        .use_global_cache_boundary = true,
    };

    auto sections = cc::constants::prompts::get_system_prompt(options);
    ASSERT_GE(sections.size(), 6u);

    auto joined = std::accumulate(std::next(sections.begin()), sections.end(), sections.front(),
        [](std::string acc, const std::string& section) {
            acc += "\n";
            acc += section;
            return acc;
        });

    EXPECT_NE(joined.find("You are Claude Code"), std::string::npos);
    EXPECT_NE(joined.find("# Tone and style"), std::string::npos);
    EXPECT_NE(joined.find(cc::constants::prompts::system_prompt_dynamic_boundary), std::string::npos);
    EXPECT_NE(joined.find("# Environment"), std::string::npos);
    EXPECT_NE(joined.find("Read"), std::string::npos);
    EXPECT_NE(joined.find("Write"), std::string::npos);
}
