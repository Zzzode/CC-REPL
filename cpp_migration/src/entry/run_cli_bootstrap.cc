/**
 * CC-REPL service-layer bootstrap (TU3 — Phase C-c split)
 *
 * Stub translation unit.  In Phase 3 this TU will own the named-module
 * imports for the non-UI service layer (daemon, bridge, server, headless
 * SSE/websocket, etc.).  For Phase C-c we deliberately import nothing and
 * return 0 so the slim main.cc's trailing dispatch has a valid target.
 *
 * The entry point is exposed with C linkage so TU1 (main.cc) can call it
 * without importing any C++ named modules.
 */

extern "C" int run_cli_bootstrap(int /*argc*/, char** /*argv*/) {
    // Phase 3: import cc.daemon.*, cc.bridge.*, cc.server.*, cc.cli.*
    // here and wire up --server / --bridge-daemon / --headless etc.
    return 0;
}
