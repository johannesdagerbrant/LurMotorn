# modules/save — persistent key -> blob store + device identity (issue #17). Pure
# C++ std::filesystem, so it compiles verbatim on host / Android (NDK) / iOS.
add_library(lur_save Private/Store.cpp Private/DeviceId.cpp)
target_include_directories(lur_save PUBLIC Public)
target_compile_features(lur_save PUBLIC cxx_std_20)
add_library(lur::save ALIAS lur_save)
