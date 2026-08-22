cmake_minimum_required(VERSION 3.25)

project(insta360_linkd VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LINKD_BUILD_TESTS "Build unit tests" ON)
option(LINKD_ENABLE_GRPC "Build the gRPC control server" ON)

include(FetchContent)
find_package(Threads REQUIRED)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)

set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)

if(LINKD_ENABLE_GRPC)
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(CARES_INSTALL OFF CACHE BOOL "" FORCE)
    set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
    set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_CODEGEN ON CACHE BOOL "" FORCE)
    set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
    set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG 8e37db5e797b6167f3a00d697d816a684bd259c7
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG d66d9a95997d51a8ba9822a611d1267757741535
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 65ee68451d8eb2b5f3a30b410476ab83deb3289b
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3 httplib nlohmann_json)

if(LINKD_ENABLE_GRPC)
    FetchContent_Declare(
        grpc
        GIT_REPOSITORY https://github.com/grpc/grpc.git
        GIT_TAG v1.83.0
        GIT_SHALLOW TRUE
        GIT_SUBMODULES
            third_party/abseil-cpp
            third_party/boringssl-with-bazel
            third_party/cares/cares
            third_party/protobuf
            third_party/re2
            third_party/zlib
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_MakeAvailable(grpc)
endif()

add_library(linkd_core
    src/control.cpp
    src/controller.cpp
)

target_include_directories(linkd_core PUBLIC include)
target_compile_features(linkd_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(linkd_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(linkd_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(APPLE)
    add_library(linkd_platform src/platform/macos_uvc_camera.cpp)
    target_link_libraries(linkd_platform PUBLIC linkd_core PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit"
    )
elseif(UNIX)
    add_library(linkd_platform src/platform/linux_v4l2_camera.cpp)
    target_link_libraries(linkd_platform PUBLIC linkd_core)
else()
    add_library(linkd_platform src/platform/unsupported_camera.cpp)
    target_link_libraries(linkd_platform PUBLIC linkd_core)
endif()

target_include_directories(linkd_platform PUBLIC include)
target_compile_features(linkd_platform PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(linkd_platform PRIVATE /W4 /permissive-)
else()
    target_compile_options(linkd_platform PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(LINKD_ENABLE_GRPC)
    set(LINKD_PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/proto")
    set(LINKD_PROTO_FILE "${LINKD_PROTO_ROOT}/linkd/v1/control.proto")
    set(LINKD_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(LINKD_PROTO_SOURCE "${LINKD_GENERATED_DIR}/linkd/v1/control.pb.cc")
    set(LINKD_PROTO_HEADER "${LINKD_GENERATED_DIR}/linkd/v1/control.pb.h")
    set(LINKD_GRPC_SOURCE "${LINKD_GENERATED_DIR}/linkd/v1/control.grpc.pb.cc")
    set(LINKD_GRPC_HEADER "${LINKD_GENERATED_DIR}/linkd/v1/control.grpc.pb.h")

    add_custom_command(
        OUTPUT
            "${LINKD_PROTO_SOURCE}"
            "${LINKD_PROTO_HEADER}"
            "${LINKD_GRPC_SOURCE}"
            "${LINKD_GRPC_HEADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${LINKD_GENERATED_DIR}"
        COMMAND $<TARGET_FILE:protobuf::protoc>
        ARGS
            "--proto_path=${LINKD_PROTO_ROOT}"
            "--cpp_out=${LINKD_GENERATED_DIR}"
            "--grpc_out=${LINKD_GENERATED_DIR}"
            "--plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>"
            "${LINKD_PROTO_FILE}"
        DEPENDS
            "${LINKD_PROTO_FILE}"
            protobuf::protoc
            grpc_cpp_plugin
        VERBATIM
    )

    add_library(linkd_grpc
        src/grpc_server.cpp
        "${LINKD_PROTO_SOURCE}"
        "${LINKD_GRPC_SOURCE}"
    )
    target_include_directories(linkd_grpc PUBLIC include "${LINKD_GENERATED_DIR}")
    target_compile_features(linkd_grpc PUBLIC cxx_std_20)
    target_link_libraries(linkd_grpc PUBLIC
        linkd_core
        grpc++
        grpc++_reflection
        protobuf::libprotobuf
    )
else()
    add_library(linkd_grpc src/grpc_server_disabled.cpp)
    target_include_directories(linkd_grpc PUBLIC include)
    target_compile_features(linkd_grpc PUBLIC cxx_std_20)
    target_link_libraries(linkd_grpc PUBLIC linkd_core)
endif()

add_executable(linkd
    src/gamepad.cpp
    src/main.cpp
    src/server.cpp
)
target_link_libraries(linkd PRIVATE
    linkd_platform
    linkd_grpc
    SDL3::SDL3-static
    httplib::httplib
    nlohmann_json::nlohmann_json
    Threads::Threads
)

install(TARGETS linkd RUNTIME DESTINATION bin COMPONENT linkd)

if(MSVC)
    target_compile_options(linkd PRIVATE /W4 /permissive-)
else()
    target_compile_options(linkd PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()


if(LINKD_BUILD_TESTS)
    enable_testing()
    add_executable(linkd_core_tests tests/control_test.cpp)
    target_link_libraries(linkd_core_tests PRIVATE linkd_core)
    add_test(NAME linkd_core_tests COMMAND linkd_core_tests)

    if(LINKD_ENABLE_GRPC)
        add_executable(linkd_grpc_tests tests/grpc_integration_test.cpp)
        target_link_libraries(linkd_grpc_tests PRIVATE linkd_grpc)
        add_test(NAME linkd_grpc_tests COMMAND linkd_grpc_tests)
    endif()
endif()
