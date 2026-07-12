# Host correctness tests for the chess rules engine (perft + codec round-trip).
add_executable(chess_rules_tests ChessRulesTests.cpp)
target_link_libraries(chess_rules_tests PRIVATE chess::core lur::serialization)
target_compile_features(chess_rules_tests PRIVATE cxx_std_20)

add_test(NAME chess_rules_tests COMMAND chess_rules_tests)

# Host tests for the Phase B game state (record codec, identity colour, merge).
add_executable(chess_state_tests ChessStateTests.cpp)
target_link_libraries(chess_state_tests PRIVATE chess::core)
target_compile_features(chess_state_tests PRIVATE cxx_std_20)

add_test(NAME chess_state_tests COMMAND chess_state_tests)
