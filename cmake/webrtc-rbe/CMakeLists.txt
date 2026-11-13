# Minimal pinned receive estimator subset. No codecs, rendering or network engine.
if(NOT UU_PYTHON)
    message(FATAL_ERROR "Python is required to verify pinned RBE sources")
endif()
execute_process(COMMAND "${UU_PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/fetch_rbe_sources.py"
    --root "${UU_RBE_SOURCE_DIR}" --verify-only
    RESULT_VARIABLE UU_RBE_VERIFY_RESULT OUTPUT_VARIABLE UU_RBE_VERIFY_OUTPUT ERROR_VARIABLE UU_RBE_VERIFY_ERROR)
if(NOT UU_RBE_VERIFY_RESULT EQUAL 0)
    message(FATAL_ERROR "Pinned RBE source verification failed:\n${UU_RBE_VERIFY_ERROR}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/source-verification.json" "${UU_RBE_VERIFY_OUTPUT}")
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-include> $<$<COMPILE_LANGUAGE:CXX>:cstdint>)
add_subdirectory("${UU_RBE_SOURCE_DIR}/abseil" "${CMAKE_CURRENT_BINARY_DIR}/abseil" EXCLUDE_FROM_ALL)
include(sources.cmake)
list(TRANSFORM UU_RBE_UPSTREAM_SOURCES PREPEND "${UU_RBE_SOURCE_DIR}/webrtc/")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/experiments")
set(UU_RBE_REGISTRY "${CMAKE_CURRENT_BINARY_DIR}/generated/experiments/registered_field_trials.h")
add_custom_command(OUTPUT "${UU_RBE_REGISTRY}"
    COMMAND "${UU_PYTHON}" "${UU_RBE_SOURCE_DIR}/webrtc/experiments/field_trials.py"
        header --output "${UU_RBE_REGISTRY}"
    DEPENDS "${UU_RBE_SOURCE_DIR}/webrtc/experiments/field_trials.py" VERBATIM)
add_library(webrtc_rbe STATIC ${UU_RBE_UPSTREAM_SOURCES} "${UU_RBE_REGISTRY}" platform_thread_types.cpp)
target_include_directories(webrtc_rbe SYSTEM PUBLIC "${UU_RBE_SOURCE_DIR}/webrtc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(webrtc_rbe PUBLIC BWE_TEST_LOGGING_COMPILE_TIME_ENABLE=0 RTC_DISABLE_LOGGING)
if(WIN32 AND NOT UU_SWITCH)
    target_compile_definitions(webrtc_rbe PUBLIC WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(webrtc_rbe PUBLIC winmm ws2_32)
else()
    target_compile_definitions(webrtc_rbe PUBLIC WEBRTC_POSIX)
endif()
target_link_libraries(webrtc_rbe PUBLIC absl::strings absl::optional)
