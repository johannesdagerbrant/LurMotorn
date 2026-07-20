# modules/trace — named CPU scopes (Insights-style), issue #101. Pure C++, no deps,
# leaf module (anything may use it without a dependency cycle). Compiles verbatim on
# host / Android (NDK) / iOS. The LUR_TRACE_SCOPE/_LATENCY macros compile to nothing
# when LUR_TRACE=0 (Shipping), so linking this in a Shipping build is inert.
add_library(lur_trace Private/Trace.cpp)
target_include_directories(lur_trace PUBLIC Public)
target_compile_features(lur_trace PUBLIC cxx_std_20)
add_library(lur::trace ALIAS lur_trace)
