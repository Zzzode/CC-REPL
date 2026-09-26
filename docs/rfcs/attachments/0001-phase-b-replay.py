#!/usr/bin/env python3
"""Cumulative Phase B replay: starts from the LIVE module graph parsed by
graph_check.load_units(), applies each execution batch as explicit module
graph edits, and after every batch runs:
  1. Tarjan on the full module graph (must stay a DAG);
  2. non-contract upward-edge diff vs the frozen live baseline (no NEW);
  3. Tarjan over the 9 TARGET_AREAS (CORE8 + cc.orchestration).
Also builds the modeled CMake target link graph before/after and checks
for library-level link cycles.
"""
import copy, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "arch"))
import graph_check as gc

ALLOW = gc.load_allowlist()
LIVE_BASELINE = gc.load_baseline()  # 13 frozen illegal upward pairs

units = gc.load_units()
LIVE = gc.module_deps(units)
g = {m: set(s) for m, s in LIVE.items()}

def add(mod, imp): g.setdefault(mod, set()).add(imp)
def drop(mod, imp):
    if mod in g: g[mod].discard(imp)
def newmod(mod, imps=()):
    g[mod] = set(imps)
def kill(mod):
    g.pop(mod, None)
    for m in g: g[m].discard(mod)
def rename(old, new):
    """Rename a module; rewrite every surviving importer; preserve edges."""
    imps = g.pop(old, set())
    g[new] = imps
    for m in g:
        if old in g[m]:
            g[m].discard(old); g[m].add(new)

def area_sccs9():
    tg = {a: set() for a in gc.TARGET_AREAS}
    for m, imps in g.items():
        a = gc.area_of(m)
        if a not in tg: continue
        for i in imps:
            b = gc.area_of(i)
            if b in tg and b != a: tg[a].add(b)
    sccs = [sorted(c) for c in gc.tarjan_scc(tg) if len(c) > 1]
    return tg, sccs

def gate_state(tag):
    cyc = [c for c in gc.tarjan_scc(g) if len(c) > 1]
    up = []
    for m in sorted(g):
        ra = gc.rank_of(m)
        if ra is None: continue
        for i in sorted(g[m]):
            rb = gc.rank_of(i)
            if rb is not None and rb > ra and not gc.is_contract(i, ALLOW):
                up.append((m, i))
    new_up = sorted(set(up) - LIVE_BASELINE)
    tg, sccs = area_sccs9()
    print(f"[{tag}] modcycles={len(cyc)} newUp={len(new_up)} "
          f"illegalUp={len(up)} targetSCCs={sccs if sccs else 'none'} "
          f"pass9={not sccs}")
    if cyc: print("   !! MODULE CYCLE", cyc[:2])
    if new_up: print("   !! NEW UPWARD", new_up)
    return not cyc and not new_up and not sccs

print("=== LIVE ===")
gate_state("live")

# ---- B1: F9 delete four zero-importer dead modules + dead services helper
for m in ["cc.commands.mcp.add_command", "cc.cli.handlers.mcp_handler",
          "cc.entrypoints.mcp_entrypoint",
          "cc.ui.features.mcp.mcp_settings_panel"]:
    kill(m)
gate_state("B1 F9-dead-modules")

# ---- B2: F9 cc.config.mcp_types leaf + config re-export
newmod("cc.config.mcp_types")
add("cc.config.config", "cc.config.mcp_types")
gate_state("B2 F9-config-leaf")

# ---- B3: F9 services alias via `export import` + field adaptations
add("cc.services.mcp.types", "cc.config.mcp_types")
gate_state("B3 F9-services-alias")

# ---- B4: F9 loader seam cuts mcp -> cc.config.config
drop("cc.tools.mcp", "cc.config.config")
add("cc.tools.mcp", "cc.config.mcp_types")
newmod("cc.commands.mcp.core_settings_loader",
       {"cc.config.config", "cc.tools.mcp"})
gate_state("B4 F9-loader-seam")

# ---- B5: F4 cc.types.tool_types atomic cut (8 created / 3 deleted)
newmod("cc.types.tool_types")
for m in ["cc.services.streaming_executor", "cc.query.query_engine",
          "cc.tools.agent.utils", "cc.tools.mcp",
          "cc.tools.runtime_message_delivery", "cc.tools.runtime_registry",
          "cc.tools.spawn_multi_agent", "cc.tools.tool"]:
    add(m, "cc.types.tool_types")
for m in ["cc.services.streaming_executor", "cc.tools.mcp",
          "cc.tools.spawn_multi_agent"]:
    drop(m, "cc.tools.tool")
gate_state("B5 F4-tool-types")

# ---- B6: F3/F8 additive snapshot sink in mcp_tool (no edge change)
gate_state("B6 F38-sink-additive")

# ---- B7: interim bridge in cc.bootstrap (outside the 9 target areas)
newmod("cc.bootstrap.mcp_connectivity", {
    "cc.hooks.remaining_notifs", "cc.services.mcp.types",
    "cc.services.mcp.connection_manager", "cc.tools.mcp"})
gate_state("B7 F38-bridge-add")

# ---- B8: atomic cut of hooks<->tools MCP legs
kill_edges = [("cc.hooks.remaining_notifs", "cc.services.mcp.types"),
              ("cc.hooks.remaining_notifs", "cc.services.mcp.connection_manager"),
              ("cc.tools.mcp", "cc.hooks.remaining_notifs")]
for a, b in kill_edges: drop(a, b)
gate_state("B8 F38-atomic-cut")

# ---- B9: CMake link hygiene (no module-graph effect)
gate_state("B9 link-hygiene")

# ---- B10: F10-A cc.skills.file_access.port + agent_resume dead import
newmod("cc.skills.file_access.port")
for m in ["cc.tools.file_read", "cc.tools.file_edit", "cc.tools.file_write"]:
    drop(m, "cc.skills.skill"); add(m, "cc.skills.file_access.port")
add("cc.skills.skill", "cc.skills.file_access.port")
drop("cc.tools.agent.resume", "cc.skills.skill")  # textually dead, deleted
gate_state("B10 F10-file-port")

# ---- B11: F10-B image codec port + cc_orchestration target appears
newmod("cc.tools.image_codec.port")
drop("cc.tools.file_read", "cc.services.image")
add("cc.tools.file_read", "cc.tools.image_codec.port")
drop("cc.tools.runtime_registry", "cc.services.image")  # computer_use TU (will move B15)
add("cc.tools.runtime_registry", "cc.tools.image_codec.port")
newmod("cc.orchestration.runtime_backends", {
    "cc.services.image", "cc.tools.image_codec.port"})
gate_state("B11 F10-codec-orch-born")

# ---- B12: F11-C skill loader executor seam
drop("cc.tools.runtime_registry", "cc.skills.skill")
newmod("cc.tools.runtime_backends.port")  # registry seam leaf (std + tool_types)
add("cc.tools.runtime_registry", "cc.tools.runtime_backends.port")
for i in ["cc.skills.skill", "cc.tools.agent_runtime",
          "cc.tools.runtime_registry", "cc.tools.tool", "cc.utils.json",
          "cc.tools.image_codec.port"]:
    add("cc.orchestration.runtime_backends", i)
gate_state("B12 F11-skill-seam")

# ---- B13: F14 Alpha1 — agent permission types sink
for m in ["cc.tools.runtime_registry", "cc.tools.team_create",
          "cc.tools.team_delete"]:
    drop(m, "cc.tools.agent"); add(m, "cc.tools.agent_types")
add("cc.tools.agent.utils", "cc.tools.agent_types")
gate_state("B13 F14a-agent-types")

# ---- B14: F14 Alpha2/3 — agent_worktree leaf, runtime_team_shared rewire
newmod("cc.tools.agent_worktree", {
    "cc.tools.agent_runtime", "cc.tools.runtime_shared_utils", "cc.utils.git"})
drop("cc.tools.runtime_team_shared", "cc.tools.agent")
add("cc.tools.runtime_team_shared", "cc.tools.agent_worktree")
add("cc.tools.agent.utils", "cc.tools.agent_worktree")  # facade re-export alias
gate_state("B14 F14a-worktree")

# ---- B15: B11 ATOMIC flip (F12+F13+F14-Beta + bridge rehome) ----
# (a) computer_use impl TU leaves cc.tools.runtime_registry for orch
drop("cc.tools.runtime_registry", "cc.tools.image_codec.port")
for m in ["cc.tools.mcp", "cc.tools.lsp", "cc.tools.agent"]:
    drop("cc.tools.runtime_registry", m)
# (b) blanket module renames (surviving importers rewritten automatically)
rename("cc.tools.agent", "cc.orchestration.agent")
rename("cc.tools.agent.run", "cc.orchestration.agent.run")
rename("cc.tools.agent.resume", "cc.orchestration.agent.resume")
rename("cc.tools.agent.fork", "cc.orchestration.agent.fork")
rename("cc.tools.agent.utils", "cc.orchestration.agent.utils")
rename("cc.tools.mcp", "cc.orchestration.tools.mcp")
rename("cc.tools.lsp", "cc.orchestration.tools.lsp")
rename("cc.tools.spawn_multi_agent", "cc.orchestration.agent.spawn_multi_agent")
# (c) bridge re-home: bootstrap module becomes orchestration module
kill("cc.bootstrap.mcp_connectivity")
newmod("cc.orchestration.mcp_connectivity", {
    "cc.hooks.remaining_notifs", "cc.services.mcp.types",
    "cc.services.mcp.connection_manager", "cc.orchestration.tools.mcp"})
# (d) orch runtime_backends gains lsp/mcp/computer_use backend bodies
for i in ["cc.orchestration.tools.mcp", "cc.orchestration.tools.lsp",
          "cc.orchestration.agent", "cc.services.image"]:
    add("cc.orchestration.runtime_backends", i)
# moved computer_use TU: services.image now reached from orch (already added),
# registry seam + mcp renamed intra-orch
add("cc.orchestration.runtime_backends", "cc.tools.agent_types")
# (e) seam leaf importer of tool_types (executor signatures) — rank down
add("cc.tools.runtime_backends.port", "cc.types.tool_types")
ok = gate_state("B15 B11-ATOMIC")

# ---- final area adjacency dump
tg, sccs = area_sccs9()
print("\n=== FINAL 9-area adjacency ===")
for a in gc.TARGET_AREAS:
    outs = sorted(tg.get(a, ()))
    print(f"  {a:18s} -> {outs}")

# ================= CMake target link graph =================
import pathlib, re
tdir = pathlib.Path("/home/zhangdi.zode/Develop/CC-REPL/src/cmake/targets")
def parse_links():
    edges = {}
    for cf in tdir.glob("*.cmake"):
        txt = cf.read_text()
        for m in re.finditer(r"target_link_libraries\(\s*([A-Za-z0-9_]+)(.*?)\)",
                             txt, re.S):
            tgt, body = m.group(1), m.group(2)
            for tok in re.findall(r"[A-Za-z0-9_:]+", body):
                if tok.startswith("cc_"):
                    edges.setdefault(tgt, set()).add(tok)
    # loom.cmake form
    return edges
links = parse_links()
cc = {t: s for t, s in links.items()}
cyc0 = [sorted(c) for c in gc.tarjan_scc(cc) if len(c) > 1]
print("\nLIVE cmake target link cycles:", cyc0 if cyc0 else "none")

# Modeled post-B15 link edits
cy = copy.deepcopy(cc)
cy["cc_orchestration"] = {"cc_tools", "cc_services", "cc_skills_core",
                          "cc_hooks", "cc_config", "cc_utils", "cc_types"}
cy["cc_tools"] = {"cc_utils", "cc_types", "cc_skills_core", "yyjson", "uv_a"}
cy["cc_bootstrap"] = {"cc_utils", "cc_state", "cc_config", "cc_services",
                      "cc_hooks"}
for t in ["cc_commands", "cc_server", "cc_ui"]:
    cy.setdefault(t, set()).add("cc_orchestration")
cy["cc_server"].discard  # noop
cy["cc_core"] = set(cy.get("cc_core", ())) | {"cc_orchestration"}
cy["cc_skills"].discard("cc_tools")
# only cc_* nodes matter
ccn = {t: {x for x in s if x.startswith("cc_")} for t, s in cy.items()}
cyc1 = [sorted(c) for c in gc.tarjan_scc(ccn) if len(c) > 1]
print("POST-B15 cmake target link cycles:", cyc1 if cyc1 else "none")
print("orchestration linkers:",
      sorted(t for t, s in ccn.items() if "cc_orchestration" in s))

sys.exit(0 if ok and not cyc0 and not cyc1 else 1)
