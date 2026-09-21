# ─── cc_migrations: Schema Migrations ─────────────────────────────────────────
add_library(cc_migrations)
target_sources(cc_migrations
    PRIVATE
        migrations/concrete_migrations.cpp
    PUBLIC FILE_SET CXX_MODULES FILES
        migrations/concrete_migrations.cppm
        migrations/config_orchestrator.cppm
        migrations/migration_registry.cppm
        migrations/migration_runner.cppm
        migrations/schema_versions.cppm
)
target_link_libraries(cc_migrations PUBLIC cc_utils)
