/// @file command_registry_init.cpp
/// @brief Module implementation unit for `cc.commands.registry`.
///
/// This is the *implementation* translation unit for the
/// `cc.commands.registry` named module.  It provides the body of
/// `register_default_commands` which fans out to five per-group
/// implementation units (command_registry_init_{a,b,c,d,e}.cpp).
///
/// Splitting the per-command fan-out across five impl units (instead
/// of importing every concrete command module here) keeps the
/// SourceLocation footprint of each translation unit well under
/// clang's 31-bit SLOC budget.  The heaviest group is "E" which
/// imports `cc.commands.runtime_surface_commands` (a module that
/// itself imports 30+ sub-command modules); isolating it in its own
/// TU prevents the aggregate BMI load from overflowing the budget.

module cc.commands.registry;

namespace cc::commands {

void register_default_commands(CommandRegistry& registry) {
    register_group_a_commands(registry);
    register_group_b_commands(registry);
    register_group_c_commands(registry);
    register_group_d_commands(registry);
    register_group_e_commands(registry);
}

} // namespace cc::commands