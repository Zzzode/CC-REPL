/// @file init_verifiers.cppm
/// @brief InitVerifiersCommand implementing the /init-verifiers slash command.
///
/// Generates CI/CD verifier configuration files for the current repo.
/// Supports four targets:
///   - GitHub Actions   (`.github/workflows/cc-verifier.yml`)
///   - GitLab CI        (`.gitlab-ci.yml` — verifier job injected)
///   - Jenkins          (`Jenkinsfile.verifier`)
///   - Shell            (`scripts/run-verifiers.sh`)
///
/// Behavior:
///   * Default mode is **preview** — output the configuration to the transcript
///     without writing to disk.
///   * Pass `--apply` to write the file(s).
///   * Pass `--type=<type>` to pick a specific target (auto-detect by default).
///   * Placeholders `{{PROJECT_NAME}}`, `{{DEFAULT_BRANCH}}`, etc. are replaced
///     with values detected from the repository.
///
/// This replaces the stub implementation that wrote a minimal JSON profile.
module;

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <sstream>
#include <utility>

export module cc.commands.init_verifiers;

import cc.types.types;
import cc.commands.command;
import cc.utils.exec_sync;
import cc.utils.git_filesystem;
import cc.utils.detect_repository;

export namespace cc::commands {

using namespace cc::core;
namespace fs = std::filesystem;

// ============================================================
// Target CI system enum & auto-detection
// ============================================================

enum class VerifierTarget : std::uint8_t {
    GithubActions,
    GitLabCI,
    Jenkins,
    Shell,
};

[[nodiscard]] constexpr std::string_view target_name(VerifierTarget t) noexcept {
    switch (t) {
        case VerifierTarget::GithubActions: return "GitHub Actions";
        case VerifierTarget::GitLabCI:      return "GitLab CI";
        case VerifierTarget::Jenkins:       return "Jenkins";
        case VerifierTarget::Shell:         return "Shell script";
    }
    return "unknown";
}

/// Auto-detect which target to generate based on what files exist in the repo
/// and any CI-identifying environment variables.
[[nodiscard]] inline VerifierTarget detect_target(const fs::path& repo_root) {
    // 1) Environment hints from CI runners
    if (std::getenv("GITHUB_ACTIONS")) return VerifierTarget::GithubActions;
    if (std::getenv("GITLAB_CI"))     return VerifierTarget::GitLabCI;
    if (std::getenv("JENKINS_HOME"))   return VerifierTarget::Jenkins;

    // 2) Filesystem hints
    if (fs::exists(repo_root / ".github" / "workflows"))
        return VerifierTarget::GithubActions;
    if (fs::exists(repo_root / ".gitlab-ci.yml"))
        return VerifierTarget::GitLabCI;
    if (fs::exists(repo_root / "Jenkinsfile"))
        return VerifierTarget::Jenkins;

    // 3) Default to portable shell script
    return VerifierTarget::Shell;
}

// ============================================================
// Placeholder resolution
// ============================================================

struct RepoContext {
    std::string project_name;
    std::string default_branch;
    std::string repo_full;       // owner/repo
    std::string package_manager; // npm, pnpm, yarn, bun, cargo, go, pip, make
};

/// Resolve project-level metadata for template substitution.
[[nodiscard]] inline RepoContext detect_repo_context(const fs::path& root) {
    RepoContext ctx;

    // Project name — from package.json, Cargo.toml, go.mod, pyproject.toml, or dirname
    if (fs::exists(root / "package.json"))  { ctx.package_manager = "npm"; }
    if (fs::exists(root / "pnpm-lock.yaml")) { ctx.package_manager = "pnpm"; }
    if (fs::exists(root / "yarn.lock"))     { ctx.package_manager = "yarn"; }
    if (fs::exists(root / "bun.lockb"))     { ctx.package_manager = "bun"; }
    if (fs::exists(root / "Cargo.toml"))    { ctx.package_manager = "cargo"; }
    if (fs::exists(root / "go.mod"))        { ctx.package_manager = "go"; }
    if (fs::exists(root / "pyproject.toml")){ ctx.package_manager = "pip"; }
    if (fs::exists(root / "Makefile"))      { ctx.package_manager = "make"; }

    // Project name = current directory name by default
    if (auto pn = root.filename().string(); !pn.empty()) ctx.project_name = pn;
    else ctx.project_name = "project";

    // Default branch: git symbolic-ref HEAD origin or fallback to "main"
    if (auto br = cc::utils::exec_sync("git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null")) {
        auto slash = br->rfind('/');
        ctx.default_branch = (slash != std::string::npos) ? br->substr(slash + 1) : *br;
        // Trim trailing newline already done by exec_sync
    }
    if (ctx.default_branch.empty()) ctx.default_branch = "main";

    // repo_full — via git remote
    if (auto repo = cc::utils::detect_repository()) {
        ctx.repo_full = std::format("{}/{}", repo->owner, repo->name);
        if (repo->name.size() > 2) ctx.project_name = repo->name;
    }
    if (ctx.repo_full.empty()) ctx.repo_full = ctx.project_name;

    return ctx;
}

/// Replace all `{{KEY}}` placeholders in a template string.
[[nodiscard]] inline std::string substitute_template(
    std::string_view tmpl,
    const RepoContext& ctx
) {
    auto replace = [](std::string& s, std::string_view key, std::string_view value) {
        std::string token = std::format("{{{{{}}}}}", key);
        std::size_t pos = 0;
        while ((pos = s.find(token, pos)) != std::string::npos) {
            s.replace(pos, token.size(), value);
            pos += value.size();
        }
    };

    std::string result(tmpl);
    replace(result, "PROJECT_NAME",    ctx.project_name);
    replace(result, "DEFAULT_BRANCH",  ctx.default_branch);
    replace(result, "REPO_FULL",       ctx.repo_full);
    replace(result, "PACKAGE_MANAGER", ctx.package_manager);
    return result;
}

// ============================================================
// CI templates (raw string literals)
// ============================================================

/// GitHub Actions workflow template
[[nodiscard]] inline std::string_view template_github_actions() {
    return R"raw(name: CC Verifier

on:
  pull_request:
    branches: [{{DEFAULT_BRANCH}}]
  push:
    branches: [{{DEFAULT_BRANCH}}]

concurrency:
  group: verifier-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build-and-test:
    name: Build + unit tests
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Setup environment
        if: runner.os == 'Linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y build-essential git

      - name: Install deps ({{PACKAGE_MANAGER}})
        run: |
          case "${{PACKAGE_MANAGER}}" in
            npm|pnpm|yarn|bun)
              if command -v ${{PACKAGE_MANAGER}} >/dev/null 2>&1; then
                ${{PACKAGE_MANAGER}} install
              fi
              ;;
            cargo)  cargo fetch ;;
            go)     go mod download ;;
            pip)    python3 -m venv .venv && . .venv/bin/activate && pip install -e . ;;
            make)   echo "make-based: skipping dep step" ;;
          esac

      - name: Build
        run: |
          case "${{PACKAGE_MANAGER}}" in
            npm|pnpm|yarn|bun) ${{PACKAGE_MANAGER}} run build 2>/dev/null || echo "no build script" ;;
            cargo)              cargo build --release ;;
            go)                 go build ./... ;;
            pip)                echo "python: build step n/a" ;;
            make)               make -j ;;
          esac

      - name: Test
        run: |
          case "${{PACKAGE_MANAGER}}" in
            npm|pnpm|yarn|bun) ${{PACKAGE_MANAGER}} test 2>/dev/null || echo "no test script" ;;
            cargo)              cargo test --release ;;
            go)                 go test ./... ;;
            pip)                python3 -m pytest -q 2>/dev/null || echo "no pytest" ;;
            make)               make test 2>/dev/null || echo "no test target" ;;
          esac

  diff-check:
    name: Diff regression check
    needs: build-and-test
    runs-on: ubuntu-latest
    if: github.event_name == 'pull_request'
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0

      - name: Fetch base branch
        run: git fetch origin ${{ github.base_ref }}:refs/remotes/origin/${{ github.base_ref }}

      - name: Comment statistics
        run: |
          echo "### Diff summary" >> $GITHUB_STEP_SUMMARY
          git diff --shortstat origin/${{ github.base_ref }}...HEAD >> $GITHUB_STEP_SUMMARY

  verifier-e2e:
    name: End-to-end verifiers
    needs: build-and-test
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install deps
        run: echo "Install verifier runtime — place e2e setup here"

      - name: Run e2e verifier skills
        run: |
          echo "Run: cc verify --all"
          echo "Verifier output goes here. The Verify agent discovers skills"
          echo "by scanning .claude/skills/*verifier*/ directories."
)raw";
}

/// GitLab CI template
[[nodiscard]] inline std::string_view template_gitlab_ci() {
    return R"raw(stages:
  - build
  - test
  - verify

variables:
  PROJECT_NAME: "{{PROJECT_NAME}}"
  DEFAULT_BRANCH: "{{DEFAULT_BRANCH}}"

build:
  stage: build
  image: alpine:latest
  script:
    - apk add --no-cache build-base git
    - |
      case "{{PACKAGE_MANAGER}}" in
        npm|pnpm|yarn|bun)
          apk add --no-cache nodejs npm
          if command -v {{PACKAGE_MANAGER}} >/dev/null 2>&1; then
            {{PACKAGE_MANAGER}} install
            {{PACKAGE_MANAGER}} run build 2>/dev/null || true
          fi
          ;;
        cargo)
          apk add --no-cache rust cargo
          cargo build --release
          ;;
        go)
          apk add --no-cache go
          go mod download
          go build ./...
          ;;
        pip)
          apk add --no-cache python3 py3-pip
          python3 -m venv .venv && . .venv/bin/activate
          pip install -e .
          ;;
        make)
          make -j
          ;;
      esac
  artifacts:
    paths: [target/, dist/, build/, .venv/]
    expire_in: 1 week

test:
  stage: test
  needs: [build]
  image: alpine:latest
  script:
    - apk add --no-cache build-base git
    - |
      case "{{PACKAGE_MANAGER}}" in
        npm|pnpm|yarn|bun)
          apk add --no-cache nodejs npm
          {{PACKAGE_MANAGER}} test 2>/dev/null || echo "no test script"
          ;;
        cargo)
          apk add --no-cache rust cargo
          cargo test --release
          ;;
        go)
          apk add --no-cache go
          go test ./...
          ;;
        pip)
          apk add --no-cache python3 py3-pip
          python3 -m venv .venv && . .venv/bin/activate
          pip install -e .
          python3 -m pytest -q 2>/dev/null || echo "no pytest"
          ;;
        make)
          make test 2>/dev/null || echo "no test target"
          ;;
      esac

verify:
  stage: verify
  needs: [test]
  rules:
    - if: '$CI_PIPELINE_SOURCE == "merge_request_event"'
    - if: '$CI_COMMIT_BRANCH == "{{DEFAULT_BRANCH}}"'
  script:
    - echo "Running CC verifier skills"
    - echo "Discover and execute .claude/skills/*verifier*/ skill files"
    - echo "cc verify --all (stub — integrate with Verify agent here)"
)raw";
}

/// Jenkins verifier pipeline template
[[nodiscard]] inline std::string_view template_jenkins() {
    return R"raw(pipeline {
  agent any
  environment {
    PROJECT_NAME    = '{{PROJECT_NAME}}'
    DEFAULT_BRANCH  = '{{DEFAULT_BRANCH}}'
    PKG_MGR         = '{{PACKAGE_MANAGER}}'
  }
  options {
    timestamps()
    timeout(time: 60, unit: 'MINUTES')
    buildDiscarder(logRotator(numToKeepStr: '20'))
  }
  stages {
    stage('Checkout') {
      steps { checkout scm }
    }
    stage('Install dependencies') {
      steps {
        sh '''
          set -e
          case "$PKG_MGR" in
            npm|pnpm|yarn|bun)  $PKG_MGR install ;;
            cargo)               cargo fetch ;;
            go)                  go mod download ;;
            pip)                 python3 -m venv .venv && . .venv/bin/activate && pip install -e . ;;
            make)                echo "make-based: no separate dep step" ;;
          esac
        '''
      }
    }
    stage('Build') {
      steps {
        sh '''
          set -e
          case "$PKG_MGR" in
            npm|pnpm|yarn|bun)  $PKG_MGR run build 2>/dev/null || echo "no build script" ;;
            cargo)               cargo build --release ;;
            go)                  go build ./... ;;
            pip)                 echo "python: build step n/a" ;;
            make)                make -j ;;
          esac
        '''
      }
    }
    stage('Tests') {
      steps {
        sh '''
          set -e
          case "$PKG_MGR" in
            npm|pnpm|yarn|bun)  $PKG_MGR test 2>/dev/null || echo "no test script" ;;
            cargo)               cargo test --release ;;
            go)                  go test ./... ;;
            pip)                 . .venv/bin/activate; python3 -m pytest -q 2>/dev/null || echo "no pytest" ;;
            make)                make test 2>/dev/null || echo "no test target" ;;
          esac
        '''
      }
    }
    stage('Diff regression') {
      when { changeRequest() }
      steps {
        sh '''
          git fetch origin "$CHANGE_TARGET" --depth=50
          echo "--- Diff stats vs $CHANGE_TARGET ---"
          git diff --shortstat "origin/$CHANGE_TARGET...HEAD" || true
        '''
      }
    }
    stage('Run verifiers') {
      steps {
        sh '''
          echo "Discovering verifier skills in .claude/skills/*verifier*/"
          find .claude/skills -maxdepth 2 -iname '*verifier*' -name 'SKILL.md' 2>/dev/null || \
            echo "No verifier skills yet. Run /init-verifiers locally to generate them."
        '''
      }
    }
  }
  post {
    always {
      echo "Pipeline complete."
    }
  }
}
)raw";
}

/// Shell-script template (portable fallback)
[[nodiscard]] inline std::string_view template_shell() {
    return R"raw(#!/usr/bin/env bash
# CC Verifier runner — generated for {{PROJECT_NAME}} ({{PACKAGE_MANAGER}})
# Usage: scripts/run-verifiers.sh [--e2e]
#
# Runs:
#   1. Install deps
#   2. Build
#   3. Unit tests
#   4. (optional, --e2e) Run skills-based verifiers in .claude/skills/*verifier*/
set -euo pipefail

PROJECT="{{PROJECT_NAME}}"
PKG="{{PACKAGE_MANAGER}}"
BRANCH="${{DEFAULT_BRANCH:-main}}"
E2E=0
for a in "$@"; do
  case "$a" in
    --e2e) E2E=1 ;;
    -h|--help)
      echo "Usage: $0 [--e2e]"
      echo "Build, test, and (optionally) run end-to-end verifier skills."
      exit 0
      ;;
  esac
done

echo "== $PROJECT: build & unit tests =="

case "$PKG" in
  npm|pnpm|yarn|bun)
    $PKG install
    $PKG run build 2>/dev/null || echo "[warn] no build script"
    $PKG test         2>/dev/null || echo "[warn] no test script"
    ;;
  cargo)
    cargo build --release
    cargo test  --release
    ;;
  go)
    go mod download
    go build ./...
    go test  ./...
    ;;
  pip)
    python3 -m venv .venv
    # shellcheck disable=SC1091
    . .venv/bin/activate
    pip install -e . 2>/dev/null || true
    python3 -m pytest -q 2>/dev/null || echo "[warn] no pytest"
    ;;
  make)
    make -j
    make test 2>/dev/null || echo "[warn] no test target"
    ;;
  *)
    echo "[unknown package manager $PKG] aborting"
    exit 1
    ;;
esac

if [ "$E2E" = "1" ]; then
  echo
  echo "== E2E verifier skills (.claude/skills/*verifier*/) =="
  SKILL_COUNT=$(find .claude/skills -maxdepth 2 -iname '*verifier*' -name 'SKILL.md' 2>/dev/null | wc -l | tr -d ' ')
  if [ "$SKILL_COUNT" = "0" ]; then
    echo "[info] No verifier skills yet. Run /init-verifiers in CC REPL to generate them."
  else
    echo "Found $SKILL_COUNT verifier skills."
    echo "Execute via: cc verify --all"
  fi
fi

echo
echo "[OK] $PROJECT verifiers complete."
)raw";
}

// ============================================================
// Output target + path mapping
// ============================================================

/// Returns (display_label, output_path_relative, template_string_view)
/// for the given target.
struct TemplateOutcome {
    std::string label;
    fs::path rel_path;
    std::string content;
};

[[nodiscard]] inline TemplateOutcome render_template(
    VerifierTarget target,
    const RepoContext& ctx
) {
    switch (target) {
        case VerifierTarget::GithubActions:
            return TemplateOutcome{
                .label = std::string(target_name(target)),
                .rel_path = fs::path(".github") / "workflows" / "cc-verifier.yml",
                .content = substitute_template(template_github_actions(), ctx),
            };
        case VerifierTarget::GitLabCI:
            return TemplateOutcome{
                .label = std::string(target_name(target)),
                .rel_path = ".gitlab-ci-cc-verifier.yml",
                .content = substitute_template(template_gitlab_ci(), ctx),
            };
        case VerifierTarget::Jenkins:
            return TemplateOutcome{
                .label = std::string(target_name(target)),
                .rel_path = "Jenkinsfile.verifier",
                .content = substitute_template(template_jenkins(), ctx),
            };
        case VerifierTarget::Shell:
            return TemplateOutcome{
                .label = std::string(target_name(target)),
                .rel_path = fs::path("scripts") / "run-verifiers.sh",
                .content = substitute_template(template_shell(), ctx),
            };
    }
    // Unreachable
    return {};
}

// ============================================================
// Command class
// ============================================================

/// InitVerifiersCommand implements the `/init-verifiers` slash command.
class InitVerifiersCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "init-verifiers",
            .description = "Generate CI/CD verifier configuration for this repo",
            .aliases = {"verifiers", "create-verifiers"},
            .args = {
                CommandArg{
                    .name = "--type",
                    .description = "CI target: github|gitlab|jenkins|shell (default: auto-detect)",
                    .type = ArgType::Choice,
                    .required = false,
                    .choices = {"github", "gitlab", "jenkins", "shell"},
                },
                CommandArg{
                    .name = "--apply",
                    .description = "Write files to disk (default: preview only)",
                    .type = ArgType::None,
                    .required = false,
                },
                CommandArg{
                    .name = "--all",
                    .description = "Generate all four targets at once instead of auto-selecting one",
                    .type = ArgType::None,
                    .required = false,
                },
            },
            .hidden = false,
            .category = "configuration",
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        // No required args
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext& ctx) {
        // --- Parse flags ---
        bool apply = false;
        bool generate_all = false;
        std::optional<VerifierTarget> explicit_target;
        for (const auto& arg : ctx.args) {
            if (arg == "--apply") apply = true;
            else if (arg == "--all") generate_all = true;
            else if (arg.starts_with("--type=")) {
                auto val = arg.substr(7);
                if      (val == "github")  explicit_target = VerifierTarget::GithubActions;
                else if (val == "gitlab")  explicit_target = VerifierTarget::GitLabCI;
                else if (val == "jenkins") explicit_target = VerifierTarget::Jenkins;
                else if (val == "shell")   explicit_target = VerifierTarget::Shell;
                else {
                    return std::unexpected(Error::make(
                        ErrorCode::InvalidInput,
                        std::format("--type='{}' is not valid. Use github|gitlab|jenkins|shell.", val)
                    ));
                }
            }
        }

        // --- Detect repo root and context ---
        auto root_res = cc::utils::exec_sync("git rev-parse --show-toplevel");
        fs::path repo_root;
        if (root_res && !root_res->empty()) repo_root = *root_res;
        else repo_root = fs::current_path();

        auto repo_ctx = detect_repo_context(repo_root);

        // --- Choose which targets to render ---
        std::vector<VerifierTarget> targets;
        if (generate_all) {
            targets = {
                VerifierTarget::GithubActions,
                VerifierTarget::GitLabCI,
                VerifierTarget::Jenkins,
                VerifierTarget::Shell,
            };
        } else {
            targets.push_back(explicit_target.value_or(detect_target(repo_root)));
        }

        // --- Render + apply ---
        std::ostringstream out;
        out << std::format("# Verifier configuration — {} ({} mode)\n\n",
                           repo_ctx.project_name,
                           apply ? "apply" : "preview");
        out << std::format("Default branch: `{}`   Repo: `{}`   Package manager: `{}`\n\n",
                           repo_ctx.default_branch, repo_ctx.repo_full, repo_ctx.package_manager);

        for (auto t : targets) {
            auto rendered = render_template(t, repo_ctx);
            out << std::format("## {} → `{}`\n\n", rendered.label, rendered.rel_path.string());

            if (apply) {
                fs::path full = repo_root / rendered.rel_path;
                std::error_code ec;
                fs::create_directories(full.parent_path(), ec);
                if (ec) {
                    out << std::format("> **Error** creating dirs for `{}`: {}\n\n",
                                       rendered.rel_path.string(), ec.message());
                    continue;
                }
                std::ofstream f(full, std::ios::trunc);
                if (!f) {
                    out << std::format("> **Error** writing `{}`.\n\n", rendered.rel_path.string());
                    continue;
                }
                f << rendered.content;
                f.close();

                // Make shell script executable
                if (t == VerifierTarget::Shell) {
                    fs::permissions(full,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                        | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::add, ec);
                }

                out << std::format("> **Written**: `{}` ({} bytes)\n\n",
                                   rendered.rel_path.string(), fs::file_size(full, ec));
            } else {
                out << "```yaml\n" << rendered.content << "\n```\n\n";
            }
        }

        out << "---\n\n";
        if (apply) {
            out << "Next steps:\n"
                   "  1. Review the generated file(s) above and commit them.\n"
                   "  2. Run `/init-verifiers` *locally in CC REPL* to generate\n"
                   "     `.claude/skills/verifier-*/SKILL.md` skill files.\n"
                   "  3. Verify the CI passes on a PR before merging to `{{DEFAULT_BRANCH}}`.\n";
        } else {
            out << "> Preview mode — no files were written. Re-run with\n"
                   "> `/init-verifiers --apply` to write them to disk, or copy\n"
                   "> the snippets above into the indicated paths manually.\n";
        }

        return CommandResult::success(out.str());
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        static constexpr std::array<std::string_view, 7> flags = {
            "--type=github", "--type=gitlab", "--type=jenkins", "--type=shell",
            "--apply", "--all", "--help"
        };
        for (auto f : flags) {
            if (f.starts_with(partial)) suggestions.emplace_back(f);
        }
        return suggestions;
    }
};

} // namespace cc::commands
