# Games/Chess — the chess game. core/ is pure C++ shared verbatim by the Android
# and iOS apps. It depends on engine modules but never the reverse.
add_library(chess_core
    Core/Private/Board.cpp
    Core/Private/MoveGen.cpp
    Core/Private/MoveCodec.cpp
    Core/Private/ChessRecord.cpp
    Core/Private/ChessMatchState.cpp
    Core/Private/OpponentRegistry.cpp
    Core/Private/MatchMeta.cpp)

target_include_directories(chess_core PUBLIC Core/Public PRIVATE Core/Private)
# lur::save for the ISaveState contract ChessMatchState implements (issue #18).
target_link_libraries(chess_core PUBLIC lur::serialization lur::save)
target_compile_features(chess_core PUBLIC cxx_std_20)
add_library(chess::core ALIAS chess_core)

# View — the board's presentation + interaction, shared by the Android and iOS
# apps. Talks only to the IRenderer interface (no Vulkan), so it builds on the
# host too. PieceMasks.h (cooked piece silhouettes) lives in Private.
add_library(chess_view
    View/Private/BoardView.cpp)
target_include_directories(chess_view PUBLIC View/Public PRIVATE View/Private)
target_link_libraries(chess_view PUBLIC chess::core lur::render lur::net lur::hud)
target_compile_features(chess_view PUBLIC cxx_std_20)
add_library(chess::view ALIAS chess_view)
