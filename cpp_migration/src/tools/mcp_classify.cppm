module;
#include <string>
#include <string_view>
#include <cstddef>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <ranges>

export module cc.tools.mcp_classify;

export namespace cc::tools {


enum class McpOutputType {
    Text,
    Code,
    Table,
    Error,
    Progress,
    Unknown
};


inline auto classify_mcp_output(std::string_view content) -> McpOutputType {
    if (content.empty()) {
        return McpOutputType::Unknown;
    }


    if (content.starts_with("Error:") ||
        content.starts_with("error:") ||
        content.starts_with("ERROR") ||
        content.starts_with("fatal:") ||
        content.starts_with("Failed")) {
        return McpOutputType::Error;
    }


    if (content.find("...") != std::string_view::npos &&
        content.size() < 200) {
        return McpOutputType::Progress;
    }
    if (content.starts_with("[") && content.find(']') < 10) {

        return McpOutputType::Progress;
    }


    size_t code_indicators = 0;
    if (content.find("```") != std::string_view::npos) code_indicators += 3;
    if (content.find("    ") != std::string_view::npos) code_indicators += 1;
    if (content.find("function ") != std::string_view::npos) code_indicators += 2;
    if (content.find("class ") != std::string_view::npos) code_indicators += 2;
    if (content.find("import ") != std::string_view::npos) code_indicators += 1;
    if (content.find("const ") != std::string_view::npos) code_indicators += 1;
    if (content.find("return ") != std::string_view::npos) code_indicators += 1;
    if (content.find('{') != std::string_view::npos &&
        content.find('}') != std::string_view::npos) code_indicators += 1;

    if (code_indicators >= 3) {
        return McpOutputType::Code;
    }


    if (content.find('|') != std::string_view::npos &&
        content.find("---") != std::string_view::npos) {
        return McpOutputType::Table;
    }

    size_t tab_count = std::count(content.begin(), content.end(), '\t');
    size_t line_count = std::count(content.begin(), content.end(), '\n');
    if (line_count > 2 && tab_count > line_count) {
        return McpOutputType::Table;
    }

    return McpOutputType::Text;
}


inline auto should_collapse_output(McpOutputType type, size_t length) -> bool {
    switch (type) {
        case McpOutputType::Text:
            return length > 2000;
        case McpOutputType::Code:
            return length > 5000;
        case McpOutputType::Table:
            return length > 3000;
        case McpOutputType::Error:
            return length > 1000;
        case McpOutputType::Progress:
            return false;
        case McpOutputType::Unknown:
            return length > 2000;
    }
    return length > 2000;
}


inline auto truncate_mcp_output(std::string_view content, size_t max_length) -> std::string {
    if (content.size() <= max_length) {
        return std::string(content);
    }


    static constexpr std::string_view truncation_marker =
        "\n\n... [output truncated, {} bytes omitted] ...\n\n";


    size_t head_size = max_length * 2 / 5;
    size_t tail_size = max_length * 2 / 5;
    size_t omitted = content.size() - head_size - tail_size;

    std::string result;
    result.reserve(max_length + 100);
    result.append(content.substr(0, head_size));
    result.append("\n\n... [output truncated, ");
    result.append(std::to_string(omitted));
    result.append(" bytes omitted] ...\n\n");
    result.append(content.substr(content.size() - tail_size));

    return result;
}

// =========================================================================
// Collapse decision based on MCP tool name allowlists (classifyForCollapse.ts)
//
// Two modes of UI collapsing for MCP tool-call results:
//   * Search/Read tools  — their output is typically long listings that the
//     model already consumed verbatim; the user rarely needs to re-read them
//     so they are "auto-collapsed" by default.
//   * Everything else    — mutating operations (send_message, create_*,
//     update_*, delete_*) are always shown so the user can confirm that
//     the correct side effect happened.
//
// Matching is keyed on the tool name alone (after normalising camelCase and
// kebab-case variants to snake_case_lower).  Unknown tool names are kept
// always-visible (conservative default).
// =========================================================================

/// UI collapse hint for an MCP tool-call result.
enum class CollapseDecision {
    AlwaysShow,     ///< Never collapse (side effects, mutations, unknown tools)
    AutoCollapse,   ///< Collapse by default but user can expand (search/read)
    AlwaysCollapse, ///< Always collapse (uninteresting / verbose noise)
};

/// Forward declarations used by classify_for_collapse.
struct McpToolCallRef {
    std::string_view server_name;
    std::string_view tool_name;
};
struct McpToolResultRef {
    std::string_view content;
    bool is_error{false};
};

// ---------------------------------------------------------------------------
// Normalise a tool name to snake_case_lower.
// Mirrors TS: replace([a-z][A-Z] with _) + replace(- with _) + toLowerCase()
// ---------------------------------------------------------------------------
inline auto normalize_tool_name(std::string_view name) -> std::string {
    std::string out;
    out.reserve(name.size() + 4);
    for (std::size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (i > 0 && std::islower(static_cast<unsigned char>(name[i - 1])) &&
                     std::isupper(static_cast<unsigned char>(c))) {
            out.push_back('_');
        }
        if (c == '-' || c == '.' || c == ' ') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Search tools — classified by the "search / find / lookup" verb.
// ---------------------------------------------------------------------------
inline auto search_tool_names() -> const std::unordered_set<std::string>& {
    static const std::unordered_set<std::string> names = {
        // Slack
        "slack_search_public",
        "slack_search_public_and_private",
        "slack_search_channels",
        "slack_search_users",
        // GitHub
        "search_code",
        "search_repositories",
        "search_issues",
        "search_pull_requests",
        "search_orgs",
        "search_users",
        // Linear
        "search_documentation",
        // Datadog
        "search_logs",
        "search_spans",
        "search_rum_events",
        "search_audit_logs",
        "search_monitors",
        "search_monitor_groups",
        "find_slow_spans",
        "find_monitors_matching_pattern",
        // Sentry
        "search_docs",
        "search_events",
        "search_issue_events",
        "find_organizations",
        "find_teams",
        "find_projects",
        "find_releases",
        "find_dsns",
        // Notion
        "search",
        // Gmail
        "gmail_search_messages",
        // Google Drive
        "google_drive_search",
        // Google Calendar
        "gcal_find_my_free_time",
        "gcal_find_meeting_times",
        "gcal_find_user_emails",
        // Atlassian/Jira
        "search_jira_issues_using_jql",
        "search_confluence_using_cql",
        "lookup_jira_account_id",
        // Community Atlassian
        "confluence_search",
        "jira_search",
        "jira_search_fields",
        // Asana
        "asana_search_tasks",
        "asana_typeahead_search",
        // Filesystem
        "search_files",
        // Memory
        "search_nodes",
        // Brave Search
        "brave_web_search",
        "brave_local_search",
        // Grafana
        "search_dashboards",
        "search_folders",
        // Supabase
        "search_docs",
        // Stripe
        "search_stripe_resources",
        "search_stripe_documentation",
        // PubMed
        "search_articles",
        "find_related_articles",
        "lookup_article_by_citation",
        "search_papers",
        "search_pubmed",
        "search_pubmed_key_words",
        "search_pubmed_advanced",
        "pubmed_search",
        "pubmed_mesh_lookup",
        // Firecrawl
        "firecrawl_search",
        // Exa
        "web_search_exa",
        "web_search_advanced_exa",
        "people_search_exa",
        "linkedin_search_exa",
        "deep_search_exa",
        // Perplexity
        "perplexity_search",
        "perplexity_search_web",
        // Tavily
        "tavily_search",
        // Obsidian
        "obsidian_simple_search",
        "obsidian_complex_search",
        // MongoDB
        "find",
        "search_knowledge",
        // Neo4j
        "search_memories",
        "find_memories_by_name",
        // Airtable
        "search_records",
        // Todoist
        "find_tasks",
        "find_tasks_by_date",
        "find_completed_tasks",
        "find_projects",
        "find_sections",
        "find_comments",
        "find_project_collaborators",
        "find_activity",
        "find_labels",
        "find_filters",
        // AWS
        "search_documentation",
        "search_catalog",
        // Terraform
        "search_modules",
        "search_providers",
        "search_policies",
    };
    return names;
}

// ---------------------------------------------------------------------------
// Read / list / get / query tools.
// Listing all stable tool names from the most common MCP servers.
// ---------------------------------------------------------------------------
inline auto read_tool_names() -> const std::unordered_set<std::string>& {
    static const std::unordered_set<std::string> names = {
        // Slack
        "slack_read_channel",
        "slack_read_thread",
        "slack_read_canvas",
        "slack_read_user_profile",
        "slack_list_channels",
        "slack_get_channel_history",
        "slack_get_thread_replies",
        "slack_get_users",
        "slack_get_user_profile",
        // GitHub
        "get_me",
        "get_team_members",
        "get_teams",
        "get_commit",
        "get_file_contents",
        "get_repository_tree",
        "list_branches",
        "list_commits",
        "list_releases",
        "list_tags",
        "get_latest_release",
        "get_release_by_tag",
        "get_tag",
        "list_issues",
        "issue_read",
        "list_issue_types",
        "get_label",
        "list_label",
        "pull_request_read",
        "get_gist",
        "list_gists",
        "list_notifications",
        "get_notification_details",
        "projects_list",
        "projects_get",
        "actions_get",
        "actions_list",
        "get_job_logs",
        "get_code_scanning_alert",
        "list_code_scanning_alerts",
        "get_dependabot_alert",
        "list_dependabot_alerts",
        "get_secret_scanning_alert",
        "list_secret_scanning_alerts",
        "get_global_security_advisory",
        "list_global_security_advisories",
        "list_org_repository_security_advisories",
        "list_repository_security_advisories",
        "get_discussion",
        "get_discussion_comments",
        "list_discussion_categories",
        "list_discussions",
        "list_starred_repositories",
        "get_issue",
        "get_pull_request",
        "list_pull_requests",
        "get_pull_request_files",
        "get_pull_request_status",
        "get_pull_request_comments",
        "get_pull_request_reviews",
        // Linear
        "list_comments",
        "list_cycles",
        "get_document",
        "list_documents",
        "list_issue_statuses",
        "get_issue_status",
        "list_my_issues",
        "list_issue_labels",
        "list_projects",
        "get_project",
        "list_project_labels",
        "list_teams",
        "get_team",
        "list_users",
        "get_user",
        // Datadog
        "aggregate_logs",
        "list_spans",
        "aggregate_spans",
        "analyze_trace",
        "trace_critical_path",
        "query_metrics",
        "aggregate_rum_events",
        "list_rum_metrics",
        "get_rum_metric",
        "list_monitors",
        "get_monitor",
        "check_can_delete_monitor",
        "validate_monitor",
        "validate_existing_monitor",
        "list_dashboards",
        "get_dashboard",
        "query_dashboard_widget",
        "list_notebooks",
        "get_notebook",
        "query_notebook_cell",
        "get_profiling_metrics",
        "compare_profiling_metrics",
        // Sentry
        "whoami",
        "get_issue_details",
        "get_issue_tag_values",
        "get_trace_details",
        "get_event_attachment",
        "get_doc",
        "get_sentry_resource",
        "list_events",
        "list_issue_events",
        "get_sentry_issue",
        // Notion
        "fetch",
        "get_comments",
        "get_users",
        "get_self",
        // Gmail
        "gmail_get_profile",
        "gmail_read_message",
        "gmail_read_thread",
        "gmail_list_drafts",
        "gmail_list_labels",
        // Google Drive
        "google_drive_fetch",
        "google_drive_export",
        // Google Calendar
        "gcal_list_calendars",
        "gcal_list_events",
        "gcal_get_event",
        // Atlassian/Jira
        "atlassian_user_info",
        "get_accessible_atlassian_resources",
        "get_visible_jira_projects",
        "get_jira_project_issue_types_metadata",
        "get_jira_issue",
        "get_transitions_for_jira_issue",
        "get_jira_issue_remote_issue_links",
        "get_confluence_spaces",
        "get_confluence_page",
        "get_pages_in_confluence_space",
        "get_confluence_page_ancestors",
        "get_confluence_page_descendants",
        "get_confluence_page_footer_comments",
        "get_confluence_page_inline_comments",
        // Community Atlassian
        "confluence_get_page",
        "confluence_get_page_children",
        "confluence_get_comments",
        "confluence_get_labels",
        "jira_get_issue",
        "jira_get_transitions",
        "jira_get_worklog",
        "jira_get_agile_boards",
        "jira_get_board_issues",
        "jira_get_sprints_from_board",
        "jira_get_sprint_issues",
        "jira_get_link_types",
        "jira_download_attachments",
        "jira_batch_get_changelogs",
        "jira_get_user_profile",
        "jira_get_project_issues",
        "jira_get_project_versions",
        // Asana
        "asana_get_attachment",
        "asana_get_attachments_for_object",
        "asana_get_goal",
        "asana_get_goals",
        "asana_get_parent_goals_for_goal",
        "asana_get_portfolio",
        "asana_get_portfolios",
        "asana_get_items_for_portfolio",
        "asana_get_project",
        "asana_get_projects",
        "asana_get_project_sections",
        "asana_get_project_status",
        "asana_get_project_statuses",
        "asana_get_project_task_counts",
        "asana_get_projects_for_team",
        "asana_get_projects_for_workspace",
        "asana_get_task",
        "asana_get_tasks",
        "asana_get_stories_for_task",
        "asana_get_teams_for_workspace",
        "asana_get_teams_for_user",
        "asana_get_team_users",
        "asana_get_time_period",
        "asana_get_time_periods",
        "asana_get_user",
        "asana_get_workspace_users",
        "asana_list_workspaces",
        // Filesystem
        "read_file",
        "read_text_file",
        "read_media_file",
        "read_multiple_files",
        "list_directory",
        "list_directory_with_sizes",
        "directory_tree",
        "get_file_info",
        "list_allowed_directories",
        // Memory
        "read_graph",
        "open_nodes",
        // Postgres
        "query",
        // SQLite
        "read_query",
        "list_tables",
        "describe_table",
        // Git
        "git_status",
        "git_diff",
        "git_diff_unstaged",
        "git_diff_staged",
        "git_log",
        "git_show",
        "git_branch",
        // Grafana
        "list_teams",
        "list_users_by_org",
        "get_dashboard_by_uid",
        "get_dashboard_summary",
        "get_dashboard_property",
        "get_dashboard_panel_queries",
        "run_panel_query",
        "list_datasources",
        "get_datasource",
        "get_query_examples",
        "query_prometheus",
        "query_prometheus_histogram",
        "list_prometheus_metric_metadata",
        "list_prometheus_metric_names",
        "list_prometheus_label_names",
        "list_prometheus_label_values",
        "query_loki_logs",
        "query_loki_stats",
        "query_loki_patterns",
        "list_loki_label_names",
        "list_loki_label_values",
        "list_incidents",
        "get_incident",
        "list_sift_investigations",
        "get_sift_investigation",
        "get_sift_analysis",
        "list_oncall_schedules",
        "get_oncall_shift",
        "get_current_oncall_users",
        "list_oncall_teams",
        "list_oncall_users",
        "list_alert_groups",
        "get_alert_group",
        "get_annotations",
        "get_annotation_tags",
        "get_panel_image",
        // PagerDuty
        "list_incidents",
        "get_incident",
        "get_outlier_incident",
        "get_past_incidents",
        "get_related_incidents",
        "list_incident_notes",
        "list_incident_workflows",
        "get_incident_workflow",
        "list_services",
        "get_service",
        "list_team_members",
        "get_user_data",
        "list_schedules",
        "get_schedule",
        "list_schedule_users",
        "list_oncalls",
        "list_log_entries",
        "get_log_entry",
        "list_escalation_policies",
        "get_escalation_policy",
        "list_event_orchestrations",
        "get_event_orchestration",
        "list_status_pages",
        "get_status_page_post",
        "list_alerts_from_incident",
        "get_alert_from_incident",
        "list_change_events",
        "get_change_event",
        // Supabase
        "list_organizations",
        "get_organization",
        "get_cost",
        "list_extensions",
        "list_migrations",
        "get_logs",
        "get_advisors",
        "get_project_url",
        "get_publishable_keys",
        "generate_typescript_types",
        "list_edge_functions",
        "get_edge_function",
        "list_storage_buckets",
        "get_storage_config",
        // Stripe
        "get_stripe_account_info",
        "retrieve_balance",
        "list_customers",
        "list_products",
        "list_prices",
        "list_invoices",
        "list_payment_intents",
        "list_subscriptions",
        "list_coupons",
        "list_disputes",
        "fetch_stripe_resources",
        // PubMed
        "get_article_metadata",
        "get_full_text_article",
        "convert_article_ids",
        "get_copyright_status",
        "download_paper",
        "list_papers",
        "read_paper",
        "get_paper_fulltext",
        "get_pubmed_article_metadata",
        "download_pubmed_pdf",
        "pubmed_fetch",
        "pubmed_pmc_fetch",
        "pubmed_spell",
        "pubmed_cite",
        "pubmed_related",
        // BigQuery
        "bigquery_query",
        "bigquery_schema",
        "list_dataset_ids",
        "list_table_ids",
        "get_dataset_info",
        "get_table_info",
        // Firecrawl
        "firecrawl_scrape",
        "firecrawl_map",
        "firecrawl_crawl",
        "firecrawl_check_crawl_status",
        "firecrawl_extract",
        // Exa
        "get_code_context_exa",
        "company_research_exa",
        "crawling_exa",
        "deep_researcher_check",
        // Perplexity
        "perplexity_ask",
        "perplexity_research",
        "perplexity_reason",
        // Tavily
        "tavily_extract",
        "tavily_crawl",
        "tavily_map",
        "tavily_research",
        // Obsidian
        "obsidian_list_files_in_vault",
        "obsidian_list_files_in_dir",
        "obsidian_get_file_contents",
        "obsidian_batch_get_file_contents",
        "obsidian_get_periodic_note",
        "obsidian_get_recent_periodic_notes",
        "obsidian_get_recent_changes",
        // Figma
        "get_figma_data",
        "download_figma_images",
        // Playwright
        "browser_console_messages",
        "browser_network_requests",
        "browser_take_screenshot",
        "browser_snapshot",
        "browser_get_config",
        "browser_route_list",
        "browser_cookie_list",
        "browser_cookie_get",
        "browser_localstorage_list",
        "browser_localstorage_get",
        "browser_sessionstorage_list",
        "browser_sessionstorage_get",
        "browser_storage_state",
        // Puppeteer
        "puppeteer_screenshot",
        // MongoDB
        "list_databases",
        "list_collections",
        "collection_indexes",
        "collection_schema",
        "collection_storage_size",
        "db_stats",
        "explain",
        "mongodb_logs",
        "aggregate",
        "count",
        "export",
        // Neo4j
        "get_neo4j_schema",
        "read_neo4j_cypher",
        "list_instances",
        "get_instance_details",
        "get_instance_by_name",
        // Elasticsearch
        "list_indices",
        "get_mappings",
        "esql",
        "get_shards",
        // Airtable
        "list_records",
        "list_bases",
        "get_record",
        // Todoist
        "get_productivity_stats",
        "get_overview",
        "fetch_object",
        "user_info",
        "list_workspaces",
        "view_attachment",
        // AWS
        "get_available_services",
        "read_documentation",
        "read_sections",
        "recommend",
        "analyze_log_group",
        "analyze_metric",
        "describe_log_groups",
        "get_active_alarms",
        "get_alarm_history",
        "get_metric_data",
        "get_metric_metadata",
        // Kubernetes
        "kubectl_get",
        "kubectl_describe",
        "kubectl_logs",
        "kubectl_context",
        "explain_resource",
        "list_api_resources",
        "namespaces_list",
        "nodes_log",
        "nodes_top",
        "pods_get",
        "pods_list",
        "pods_list_in_namespace",
        "pods_log",
        "pods_top",
        "resources_get",
        "resources_list",
    };
    return names;
}

// ---------------------------------------------------------------------------
// Classify a tool-call result for UI collapsing.
//
//  1. If the tool is in the SEARCH allowlist -> AutoCollapse
//  2. Else if in the READ allowlist            -> AutoCollapse
//  3. Else (mutating / unknown)                -> AlwaysShow
//  4. Error results are always shown regardless (override)
// ---------------------------------------------------------------------------
inline auto classify_for_collapse(
    const McpToolCallRef& call,
    const McpToolResultRef& result
) -> CollapseDecision {
    // Errors are never collapsed — the user needs to see what went wrong.
    if (result.is_error) {
        return CollapseDecision::AlwaysShow;
    }

    const auto normalized = normalize_tool_name(call.tool_name);

    if (search_tool_names().contains(normalized)) {
        // Very short search results (e.g. "0 results") can stay expanded.
        if (result.content.size() < 120) {
            return CollapseDecision::AlwaysShow;
        }
        return CollapseDecision::AutoCollapse;
    }

    if (read_tool_names().contains(normalized)) {
        if (result.content.size() < 200) {
            return CollapseDecision::AlwaysShow;
        }
        return CollapseDecision::AutoCollapse;
    }

    // Unknown / mutation side-effect: always show so the user can confirm.
    return CollapseDecision::AlwaysShow;
}

/// Convenience overload returning { isSearch, isRead } to mirror TS API.
struct ClassifyFlags { bool is_search{false}; bool is_read{false}; };
inline auto classify_mcp_tool_for_collapse(
    std::string_view /*server_name*/,
    std::string_view tool_name
) -> ClassifyFlags {
    const auto normalized = normalize_tool_name(tool_name);
    return ClassifyFlags{
        .is_search = search_tool_names().contains(normalized),
        .is_read   = read_tool_names().contains(normalized),
    };
}

} // namespace cc::tools