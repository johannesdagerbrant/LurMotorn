# modules/serialization — the slim-bytes toolkit. Pure C++, zero dependencies.
add_library(lur_serialization
    Private/BitWriter.cpp
    Private/BitReader.cpp)

target_include_directories(lur_serialization PUBLIC Public PRIVATE Private)
target_compile_features(lur_serialization PUBLIC cxx_std_20)

# Engine modules live under the lur:: namespace; expose an alias so consumers
# can link `lur::serialization` regardless of where this tree is added from.
add_library(lur::serialization ALIAS lur_serialization)
