# Architecture Decision: No Runtime Dependency-Injection Container

- Date: 2026-06-15
- Status: Accepted (Decision Register entry #1)
- ADR Location: docs/why-no-di.md
- Scope: C++20 migration of the QueryEngine subsystem and all downstream
  service/service-like modules (MCP, OAuth, LSP, Hooks, Tools, State).

## Context

The original TypeScript implementation (`src/QueryEngine.ts`) exposes a
`QueryDeps` factory struct whose ~25 fields capture every collaborator the
engine depends on at construction time: an `api_client_factory`, a
`hooks_engine`, a `tools_ctx`, a `state_handle`, a `memory_directory`, a
`session_store`, a credential provider, a telemetry sink, and so on. The
pattern is deliberate in the TS codebase: it keeps the engine pure, lets the
entrypoint swap implementations for tests, and avoids pulling heavy modules
into unit-test bundles.

During the TS-to-C++20 migration, the natural question arose: should we port
`QueryDeps` to C++ as a concrete runtime struct populated with 20+
`std::function<>` factory handles and `std::shared_ptr<>` service references,
and hand that blob to every engine constructor? Several engineers with Java /
Spring backgrounds proposed introducing a minimal `boost::di`-style container
so the wiring lives in a single `injector.cppm` instead of the entrypoint.

This ADR records the decision and the rationale for rejecting that approach
for the current codebase.

## Decision

**REJECTED:** A runtime `QueryDeps` DI container, whether hand-rolled (bag of
`std::function` / `shared_ptr`) or backed by a library (boost::di, DIABLO,
etc.).

**ACCEPTED:** Native CMake-based dependency composition, explicit constructor
parameters, C++20 named-module imports, and googlemock / fakeit for test seam
substitution. In short: the build graph *is* the DI graph, and a constructor
signature *is* the dependency list.

## Rationale

### 1. No reflection in C++, so "DI containers" still require hand-wiring.

Every DI library for C++ shares one unavoidable fact with hand-rolled
solutions: without language-level reflection, every factory must be
registered explicitly, every interface bound explicitly, and every lifetime
scoped explicitly. A library removes a small amount of call-site boilerplate
but adds equal or greater boilerplate in registration macros, explicit type
lists, and module-level `install(...)` invocations. For a codebase of our
size (roughly 150 cppm files, 40 CMake targets), the net LOC saved by
introducing a container is reliably negative, and the number of places a
reader must visit to understand how a service is constructed actually
*increases* (the injector, the binding header, the module-install call, the
constructor). Explicit constructor parameters keep the whole story in one
place: the call site at the entrypoint.

### 2. `target_link_libraries` IS native dependency injection.

CMake's `target_link_libraries` with `PRIVATE|INTERFACE|PUBLIC` visibility
already gives us every correctness guarantee that a DI container is supposed
to provide, plus several it cannot. Transitive includes are enforced at
configure time: if `cc_query` depends on `cc_mcp_client` transitively and
someone forgets the chain, the build fails, not the runtime. ABI
compatibility is enforced by the linker, not by a `shared_ptr<Interface>`
whose vtable layout has silently shifted. Hermetic isolation is the default:
a test binary that links `cc_query_mock` instead of `cc_query` literally
cannot resolve the production symbol set. And, critically, the entire
composition is *zero overhead*: no vtables added where they were not
already present, no indirect function pointer calls, no per-instance
`unique_ptr<Interface>` members that defeat the optimizer. CMake's target
model is not "like" DI; it *is* the strictest possible DI system we can run
on this platform, because the linker itself is the enforcer.

### 3. Compile-time checking beats runtime checking.

A mis-wired DI container — forgotten binding, cyclic dependency, wrong
lifetime scope, mismatched interface — produces a runtime exception (or,
worse, a `nullptr` dereference) the first time the code path executes. With
the build-graph-plus-explicit-parameters approach, the same mistake produces
a compile or link error *before* the binary is produced. Cyclic dependencies
are surfaced at `cmake --build` time by Ninja/linker diagnostics. Wrong
implementations surface as `undefined reference` link errors. Lifetime
mismatches surface as compilation errors when a reference parameter cannot
bind to a temporary. This is the single most important benefit for a codebase
that is currently migrating: every configuration mistake caught by the
compiler is one configuration mistake we will never debug in a REPL session.

### 4. googlemock is strictly more powerful than runtime-swappable `std::function`.

Teams advocating a runtime DI container often cite test seam ergonomics:
swapping `RealAuthClient` for `FakeAuthClient` at test construction time is
convenient. But this advantage disappears against the capabilities of a
modern mock framework. `googlemock` provides `EXPECT_CALL` cardinality and
argument matchers, `ON_CALL` default actions, `Sequence` ordering, `NiceMock`
/ `StrictMock` rigour, and `SaveArg`/`Invoke` escape hatches — none of which
come for free with a hand-rolled `std::function<HttpResponse(const
HttpRequest&)>` swap. A `MOCK_METHOD` on an interface class covers every
case the `std::function` approach covers, plus hundreds it cannot. The
up-front cost (defining a pure-virtual interface class) is amortised the
first time you write a multi-sequence integration test, and the test code is
far more expressive as a result.

### 5. Twenty-plus `shared_ptr` / `std::function` members prevent inlining.

A `QueryDeps` bag-of-factories for 25 services imposes a real performance
tax on hot paths. Each `std::function` is a small-object container with a
virtual-trampoline call; each `shared_ptr` is a refcounted pointer with
atomic operations on construction/destruction; neither is transparent to the
optimizer. Every call through `deps_.oauth_client(request)` must go through
at least one indirect branch, which means the call cannot be inlined, its
arguments cannot be specialised, and the LTO pass loses the opportunity to
fold constant configuration values across the boundary. In contrast, an
explicit reference member `OAuthClient& oauth_` bound to a concrete
(even-via-interface) type at construction time lets the compiler devirtualise
calls, hoist loads of `oauth_` out of loops, and inline thin accessors.
Across the roughly 4M Tokens/day the query engine processes during our
internal dogfooding, the cumulative cost of the extra indirections would be
measurable; the benefit of never paying it is free under the accepted
approach.

### 6. The migrated engine already follows the accepted pattern end-to-end.

The practical case for adding a DI container is weakest when the codebase
already does not need one. `query/query_engine.cppm` (currently ~114 KB of
translation unit surface) has zero globals, zero hidden collaborators, and
every dependency is passed through its constructor or injected as a module
import with no runtime indirection. The entrypoint in `src/main.cppm`
constructs the `cc_oauth::Client`, the `cc_mcp::TransportManager`, the
`cc_hooks::Engine`, the `cc_state::AppStore`, and the `cc_tools::Registry`
concretely, then hands them as references to `QueryEngine{...}`. Test
binaries substitute `MockOAuthClient`, `FakeMcpTransport`, and so on at the
same construction site. There is literally *nothing to refactor*: the
architectural invariant the container was supposed to enforce is already the
invariant the code maintains by construction. Introducing a container would
be a net regression in simplicity, not an improvement.

## Consequences

**Positive:**

- Zero new abstractions introduced in the codebase. No injector, no binding
  DSL, no container bootstrap step in the entrypoint.
- LTO- and PGO-friendly. Reference-members and concrete targets preserve
  every devirtualisation / inlining opportunity.
- Dependency lists are always reviewable as part of the constructor
  signature. Pull-request reviewers can see a service's full graph by
  reading the constructor parameters and the `target_link_libraries` line.

**Negative:**

- Multi-permutation integration tests must hand-roll each construction site.
  Mitigation: `::testing::Test` fixtures and reusable `EngineTestBed` helper
  classes already cover 95% of the permutations we actually test; the
  remaining handful (special OAuth misconfigs, MCP transport variants) are
  rare enough that explicit construction is a feature, not a bug.

**Risk:**

- Engineers arriving from Spring / Guice / Hilt backgrounds may perceive the
  lack of a container as "not real DI" and advocate introducing one.
  Mitigation: this ADR is linked from the migration README, the
  `migration-audit-report.md`, and the C++ onboarding doc. New module
  blueprints in `docs/` show the `target_link_libraries + explicit ctor +
  MockXxx` pattern as the canonical style. Contributors raising the topic
  should be directed here rather than re-arguing the trade-offs.

## Code Examples

### CMake: target composition

```cmake
# src/query/CMakeLists.txt — production target
add_library(cc_query STATIC query_engine.cppm query_loop.cppm)
target_link_libraries(cc_query
  PRIVATE
    cc_api_client      # Anthropic API streaming
    cc_mcp_client      # MCP transport + server registry
    cc_oauth_client    # OAuth keychain + token refresh
    cc_hooks_engine    # Permission / at-mention hooks
    cc_state_store     # AppState Store + persistence
    cc_tools_registry  # Tool registry + runtime dispatch
    cc_utils_json      # Canonical JSON codec
)
target_compile_features(cc_query PRIVATE cxx_std_20)

# tests/CMakeLists.txt — test target links mocks for seams
add_executable(test_query test_query.cpp)
target_link_libraries(test_query
  PRIVATE
    cc_query               # production code under test
    gmock_main             # googlemock + gtest runner
    cc_api_client_mock     # MockApiClient <: ApiClient
    cc_mcp_client_mock     # MockMcpTransport <: McpTransport
    cc_oauth_client_mock   # MockOAuthClient <: OAuthClient
    cc_state_store_mock    # MockStore <: Store<AppState>
    cc_hooks_engine_stub   # deterministic fake hooks
)
add_test(NAME TestQuery COMMAND test_query)
```

### C++: explicit constructor + mocks

```cpp
// query/query_engine.h
import <memory>;
import cc.api.client;
import cc.mcp.client;
import cc.oauth.client;
import cc.hooks.engine;
import cc.state.store;
import cc.tools.registry;

namespace cc::query {

class QueryEngine {
public:
  QueryEngine(api::Client& api,
              mcp::Client& mcp,
              oauth::Client& oauth,
              hooks::Engine& hooks,
              state::Store<AppState>& store,
              tools::Registry& tools)
      : api_(api), mcp_(mcp), oauth_(oauth),
        hooks_(hooks), store_(store), tools_(tools) {}

  // ... run(), step(), stop() ...

private:
  api::Client&   api_;
  mcp::Client&   mcp_;
  oauth::Client& oauth_;
  hooks::Engine& hooks_;
  state::Store<AppState>& store_;
  tools::Registry& tools_;
};

}  // namespace cc::query
```

```cpp
// tests/test_query.cpp — mocks supplied at test construction
class QueryEngineSmoke : public ::testing::Test {
protected:
  StrictMock<MockApiClient>     api_;
  NiceMock<MockMcpClient>       mcp_;
  NiceMock<MockOAuthClient>     oauth_;
  NiceMock<MockHooksEngine>     hooks_;
  state::AppStore               store_{};
  tools::Registry               tools_{};
  query::QueryEngine            engine_{api_, mcp_, oauth_, hooks_, store_, tools_};
};

TEST_F(QueryEngineSmoke, ConstructsAndExposesApiCalls) {
  EXPECT_CALL(api_, Models)
      .WillOnce(Return(std::vector<api::Model>{
          {.id = "test-model", .max_tokens = 128'000}}));
  auto models = engine_.available_models();
  ASSERT_EQ(models.size(), 1u);
  EXPECT_EQ(models.front().id, "test-model");
}
```

## References

- **C++ Core Guidelines** R.30–R.37 (Smart pointers, ownership, and
  dependency injection — R.30 in particular: "Take smart pointers as
  parameters only to explicitly express lifetime semantics; prefer raw
  references/parameters for non-owning collaborators.")
- *Large-Scale C++ Volume 2: Design and Architecture*, John Lakos — Chapter 6
  on physical dependency management and the build system as the primary
  architectural enforcement layer.
- **GoogleTest Mocking Cookbook** — `StrictMock<T>`, `EXPECT_CALL`,
  `SaveArg<N>`, `Sequence`, and `DoDefault` patterns for constructing
  seam-based integration tests without container indirection.
- ISO/IEC 14882:2020 (C++20) standard — [module.import] (named modules
  isolate translation units, making concrete target-level composition the
  natural unit of dependency injection.)
