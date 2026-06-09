// WebFetch preapproved host list: code-related domains that can be fetched
// without explicit user-provided provenance. GET-only; sandbox restrictions
// deliberately do NOT inherit this list (see SECURITY WARNING in TS source).
// Mirrors src/tools/WebFetchTool/preapproved.ts
module;
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <algorithm>

export module cc.tools.web_fetch_preapproved;

export namespace cc::tools::web_fetch {

/// Returns the full set of preapproved host entries. Some entries are
/// path-scoped (e.g. "github.com/anthropics"), others are hostname-only.
[[nodiscard]] inline auto preapproved_host_entries()
    -> const std::unordered_set<std::string_view>& {
    static const std::unordered_set<std::string_view> entries = {
        // Anthropic
        "platform.claude.com",
        "code.claude.com",
        "modelcontextprotocol.io",
        "github.com/anthropics",
        "agentskills.io",

        // Top Programming Languages
        "docs.python.org",
        "en.cppreference.com",
        "docs.oracle.com",
        "learn.microsoft.com",
        "developer.mozilla.org",
        "go.dev",
        "pkg.go.dev",
        "www.php.net",
        "docs.swift.org",
        "kotlinlang.org",
        "ruby-doc.org",
        "doc.rust-lang.org",
        "www.typescriptlang.org",

        // Web & JavaScript Frameworks/Libraries
        "react.dev",
        "angular.io",
        "vuejs.org",
        "nextjs.org",
        "expressjs.com",
        "nodejs.org",
        "bun.sh",
        "jquery.com",
        "getbootstrap.com",
        "tailwindcss.com",
        "d3js.org",
        "threejs.org",
        "redux.js.org",
        "webpack.js.org",
        "jestjs.io",
        "reactrouter.com",

        // Python Frameworks & Libraries
        "docs.djangoproject.com",
        "flask.palletsprojects.com",
        "fastapi.tiangolo.com",
        "pandas.pydata.org",
        "numpy.org",
        "www.tensorflow.org",
        "pytorch.org",
        "scikit-learn.org",
        "matplotlib.org",
        "requests.readthedocs.io",
        "jupyter.org",

        // PHP Frameworks
        "laravel.com",
        "symfony.com",
        "wordpress.org",

        // Java Frameworks & Libraries
        "docs.spring.io",
        "hibernate.org",
        "tomcat.apache.org",
        "gradle.org",
        "maven.apache.org",

        // .NET & C# Frameworks
        "asp.net",
        "dotnet.microsoft.com",
        "nuget.org",
        "blazor.net",

        // Mobile Development
        "reactnative.dev",
        "docs.flutter.dev",
        "developer.apple.com",
        "developer.android.com",

        // Data Science & Machine Learning
        "keras.io",
        "spark.apache.org",
        "huggingface.co",
        "www.kaggle.com",

        // Databases
        "www.mongodb.com",
        "redis.io",
        "www.postgresql.org",
        "dev.mysql.com",
        "www.sqlite.org",
        "graphql.org",
        "prisma.io",

        // Cloud & DevOps
        "docs.aws.amazon.com",
        "cloud.google.com",
        "kubernetes.io",
        "www.docker.com",
        "www.terraform.io",
        "www.ansible.com",
        "vercel.com/docs",
        "docs.netlify.com",
        "devcenter.heroku.com",

        // Testing & Monitoring
        "cypress.io",
        "selenium.dev",

        // Game Development
        "docs.unity.com",
        "docs.unrealengine.com",

        // Other Essential Tools
        "git-scm.com",
        "nginx.org",
        "httpd.apache.org",
    };
    return entries;
}

// ---------------------------------------------------------------------------
// Internal split: hostname-only set vs. path-prefix map (computed once).
// ---------------------------------------------------------------------------
namespace detail {

struct SplitHosts {
    std::unordered_set<std::string_view> hostname_only;
    std::unordered_map<std::string_view, std::vector<std::string_view>> path_prefixes;
};

[[nodiscard]] inline auto split_hosts() -> const SplitHosts& {
    static const SplitHosts split = [] {
        SplitHosts s;
        for (const auto entry : preapproved_host_entries()) {
            const auto slash = entry.find('/');
            if (slash == std::string_view::npos) {
                s.hostname_only.insert(entry);
            } else {
                const auto host = entry.substr(0, slash);
                const auto path = entry.substr(slash);
                s.path_prefixes[host].push_back(path);
            }
        }
        return s;
    }();
    return split;
}

} // namespace detail

/// Check whether hostname (+pathname) is in the preapproved list.
/// Path-scoped entries enforce segment boundaries: "/anthropics" must not
/// match "/anthropics-evil/malware".
[[nodiscard]] inline auto is_preapproved_host(
    std::string_view hostname,
    std::string_view pathname
) -> bool {
    const auto& split = detail::split_hosts();
    if (split.hostname_only.contains(hostname)) return true;

    const auto it = split.path_prefixes.find(hostname);
    if (it == split.path_prefixes.end()) return false;

    for (const auto& prefix : it->second) {
        if (pathname == prefix) return true;
        const auto with_sep = std::string(prefix) + "/";
        if (pathname.size() >= with_sep.size() &&
            std::equal(with_sep.begin(), with_sep.end(), pathname.begin())) {
            return true;
        }
    }
    return false;
}

} // namespace cc::tools::web_fetch
