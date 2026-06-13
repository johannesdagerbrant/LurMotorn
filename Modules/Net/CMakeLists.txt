# modules/net — session lifecycle, message framing, clock sync. Pure C++.
# Header-only for now; rollback netcode will add sources here later.
add_library(lur_net INTERFACE)
target_include_directories(lur_net INTERFACE Public)
target_compile_features(lur_net INTERFACE cxx_std_20)
target_link_libraries(lur_net INTERFACE lur_transport lur_serialization)
add_library(lur::net ALIAS lur_net)
