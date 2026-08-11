# modules/app — the engine's app-level choreography (issue #43, Phase 3 of #39).
#
# GameHost today: the session + persistence flow both mains had duplicated. The rest of Phase 3
# (platform entry points, thread topology, input pipeline, agent channel) lands here too, which is
# why this is a module and not a header dropped into Net or Save — it sits ABOVE both and must be
# allowed to depend on both without either depending on it.
add_library(lur_app Private/GameHost.cpp)
target_include_directories(lur_app PUBLIC Public)
target_compile_features(lur_app PUBLIC cxx_std_20)
target_link_libraries(lur_app PUBLIC lur::net lur::save lur::transport)
add_library(lur::app ALIAS lur_app)
