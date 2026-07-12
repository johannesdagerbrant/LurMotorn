# modules/net — session lifecycle, message framing, clock sync. Pure C++.
# Compiled (Session.cpp); rollback netcode will add more sources here later.
add_library(lur_net Private/Session.cpp)
target_include_directories(lur_net PUBLIC Public)
target_compile_features(lur_net PUBLIC cxx_std_20)
target_link_libraries(lur_net PUBLIC lur_transport lur_serialization)
add_library(lur::net ALIAS lur_net)
