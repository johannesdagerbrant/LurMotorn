# modules/transport — the link abstraction. The interface is header-only and
# pure C++; the BLE backends are compiled by each app's native build (Android
# JNI->Kotlin, iOS CoreBluetooth) and are not part of this host target.
add_library(lur_transport INTERFACE)
target_include_directories(lur_transport INTERFACE Public)
target_compile_features(lur_transport INTERFACE cxx_std_20)
target_link_libraries(lur_transport INTERFACE lur::core)  # EventInbox uses LUR_ASSERT
add_library(lur::transport ALIAS lur_transport)
