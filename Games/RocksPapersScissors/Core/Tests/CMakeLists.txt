# Host correctness tests for the RPS sim core: determinism (the load-bearing
# property) + the spec's rule edges. New per-game test exe — chess's fuzz_tests as
# the pattern (there is no shared test framework; a CHECK macro + failure count).
add_executable(rps_sim_tests SimTests.cpp)
target_link_libraries(rps_sim_tests PRIVATE rps::core)
target_compile_features(rps_sim_tests PRIVATE cxx_std_20)
add_test(NAME rps_sim_tests COMMAND rps_sim_tests)

# AI opponent (#124-#126): the AiController is a pure function of sim state + tick, so it is
# host-testable without a renderer — determinism, the fixed opening, the counter choice, and
# that Hard reads the enemy more sharply than Easy.
add_executable(rps_ai_tests AiTests.cpp)
target_link_libraries(rps_ai_tests PRIVATE rps::core)
target_compile_features(rps_ai_tests PRIVATE cxx_std_20)
add_test(NAME rps_ai_tests COMMAND rps_ai_tests)

# Persistent W-L-D (ScoreBook): exercised THROUGH the real Lur::Save::Store in a temp dir, so the
# test proves the wiring a main uses and not just the byte codec (#147's lesson).
add_executable(rps_score_tests ScoreBookTests.cpp)
target_link_libraries(rps_score_tests PRIVATE rps::core lur::save)
target_compile_features(rps_score_tests PRIVATE cxx_std_20)
add_test(NAME rps_score_tests COMMAND rps_score_tests)

# The agent remote-control command grammar (LUR_AGENT). The PARSER is compiled in every build so its
# grammar is covered here rather than only in a build nobody runs; the channel that reads it and the
# effects it applies are #if LUR_AGENT. Worth testing on the host because the failure modes are
# destructive on a live phone: a command with no identity re-fires on every poll of a level-triggered
# channel, and the text arrives from outside the process.
add_executable(rps_agent_tests AgentControlTests.cpp)
target_link_libraries(rps_agent_tests PRIVATE rps::core)
target_compile_features(rps_agent_tests PRIVATE cxx_std_20)
add_test(NAME rps_agent_tests COMMAND rps_agent_tests)

# 2026-08-22: the Galaxy's 16 persisted cvar overrides became the compiled defaults. The .cfg parser
# ROUNDS and F() TRUNCATES, so baking a decimal with F instead of FRound shifts it one raw unit —
# invisible in source, real in a deterministic sim. This pins each baked default against the exact
# text that was in the .cfg.
add_executable(rps_baked_defaults_tests BakedDefaultsTests.cpp)
target_link_libraries(rps_baked_defaults_tests PRIVATE rps::core)
target_compile_features(rps_baked_defaults_tests PRIVATE cxx_std_20)
add_test(NAME rps_baked_defaults_tests COMMAND rps_baked_defaults_tests)
