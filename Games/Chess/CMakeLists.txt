# Games/Chess — the chess game. core/ is pure C++ shared verbatim by the Android
# and iOS apps. It depends on engine modules but never the reverse.
add_library(chess_core
    Core/Private/Board.cpp
    Core/Private/MoveGen.cpp
    Core/Private/MoveCodec.cpp
    Core/Private/ChessRecord.cpp
    Core/Private/ChessMatchState.cpp
    Core/Private/Captures.cpp
    Core/Private/OpponentRegistry.cpp
    Core/Private/MatchMeta.cpp)

target_include_directories(chess_core PUBLIC Core/Public PRIVATE Core/Private)
# lur::save for the ISaveState contract ChessMatchState implements (issue #18);
# lur::core for LUR_ASSERT (public: Board.h's MoveList::Add uses it).
target_link_libraries(chess_core PUBLIC lur::serialization lur::save lur::core)
target_compile_features(chess_core PUBLIC cxx_std_20)
add_library(chess::core ALIAS chess_core)

# View — the board's presentation + interaction, shared by the Android and iOS
# apps. Talks only to the IRenderer interface (no Vulkan), so it builds on the
# host too. PieceMasks.h (cooked piece silhouettes) lives in Private.
add_library(chess_view
    View/Private/BoardView.cpp
    View/Private/SfxLibrary.cpp)
target_include_directories(chess_view PUBLIC View/Public PRIVATE View/Private)
target_link_libraries(chess_view PUBLIC chess::core lur::render lur::net lur::hud lur::audio
                                      lur::devgui)  # #201: Lur::DevGui::Console (the whole tool)
target_compile_features(chess_view PUBLIC cxx_std_20)
add_library(chess::view ALIAS chess_view)

# chess::app — chess's answers to Lur::App::IGame (#45). The record-sync trio, the view attachment,
# the match-end hook and the state hash, written ONCE instead of once per platform main. Its own
# library rather than part of chess::view because a view draws and this decides, and because it needs
# lur::app — which nothing that merely renders a board should have to link.
add_library(chess_app App/Private/ChessGame.cpp)
target_include_directories(chess_app PUBLIC App/Public)
target_link_libraries(chess_app PUBLIC chess::view lur::app)
target_compile_features(chess_app PUBLIC cxx_std_20)
add_library(chess::app ALIAS chess_app)
