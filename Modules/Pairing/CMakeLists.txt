# modules/pairing — BLE discovery + confirm. Interface header-only; backends in
# the app builds (no NFC: iOS can't peer-NFC with Android).
add_library(lur_pairing INTERFACE)
target_include_directories(lur_pairing INTERFACE Public)
target_compile_features(lur_pairing INTERFACE cxx_std_20)
target_link_libraries(lur_pairing INTERFACE lur_transport)
add_library(lur::pairing ALIAS lur_pairing)
