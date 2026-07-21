# Modules/DevConsole — the dev console (spec §4). Currently the render-agnostic MODEL
# (completion, MRU, command/CVar dispatch, scrollback); the DevGui widget VIEW that draws
# it lands with the dev-GUI layer (#113). Dev-only: the root excludes this whole subdir
# from a shipping build (spec §5), and its headers lean on the dev-only DevCommand registry.
# Header-only for now; depends on Core (the CVar + DevCommand registries).
add_library(lur_devconsole INTERFACE)
target_include_directories(lur_devconsole INTERFACE Public)
target_compile_features(lur_devconsole INTERFACE cxx_std_20)
target_link_libraries(lur_devconsole INTERFACE lur::core)
add_library(lur::devconsole ALIAS lur_devconsole)
