# Games/Chess — the chess game. core/ is pure C++ shared verbatim by the Android
# and iOS apps. It depends on engine modules but never the reverse.
add_library(chess_core
    Core/Private/Board.cpp
    Core/Private/MoveGen.cpp
    Core/Private/MoveCodec.cpp)

target_include_directories(chess_core PUBLIC Core/Public PRIVATE Core/Private)
target_link_libraries(chess_core PUBLIC lur::serialization)
target_compile_features(chess_core PUBLIC cxx_std_20)
add_library(chess::core ALIAS chess_core)
