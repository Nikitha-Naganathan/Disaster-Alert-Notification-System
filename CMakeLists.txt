cmake_minimum_required(VERSION 3.14)
project(DisasterAlertSystem VERSION 1.0.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Output directories ────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Find Poco libraries ───────────────────────────────────
find_package(Poco REQUIRED COMPONENTS
    Foundation
    Net
    Util
)

if(NOT Poco_FOUND)
    message(FATAL_ERROR
        "Poco libraries not found.\n"
        "Install with:\n"
        "  Ubuntu/Debian : sudo apt install libpoco-dev\n"
        "  macOS         : brew install poco\n"
        "  Windows       : vcpkg install poco\n"
    )
endif()

# ── Common include path ───────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/common)

# ── Compiler warnings ─────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
elseif(MSVC)
    add_compile_options(/W4 /O2)
endif()

# ── Target: das_server ────────────────────────────────────
add_executable(das_server
    server/server.cpp
    common/AlertProtocol.h
    common/AlertLogger.h
    common/Utils.h
)
target_link_libraries(das_server PRIVATE
    Poco::Foundation
    Poco::Net
    Poco::Util
)

# ── Target: das_client ────────────────────────────────────
add_executable(das_client
    client/client.cpp
    common/AlertProtocol.h
    common/Utils.h
)
target_link_libraries(das_client PRIVATE
    Poco::Foundation
    Poco::Net
)

# ── Target: das_admin ─────────────────────────────────────
add_executable(das_admin
    server/admin.cpp
    common/AlertProtocol.h
    common/Utils.h
)
target_link_libraries(das_admin PRIVATE
    Poco::Foundation
    Poco::Net
)

# ── Threads ───────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(das_server PRIVATE Threads::Threads)
target_link_libraries(das_client PRIVATE Threads::Threads)
target_link_libraries(das_admin  PRIVATE Threads::Threads)

# ── Install rules ──────────────────────────────────────────
install(TARGETS das_server das_client das_admin
        RUNTIME DESTINATION bin)

# ── Print summary ──────────────────────────────────────────
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════╗")
message(STATUS "║   Disaster Alert System – Build Config   ║")
message(STATUS "╠══════════════════════════════════════════╣")
message(STATUS "║  C++ Standard : C++17                    ║")
message(STATUS "║  Build type   : ${CMAKE_BUILD_TYPE}      ")
message(STATUS "║  Poco version : ${Poco_VERSION}          ")
message(STATUS "║  Targets      : das_server               ║")
message(STATUS "║               : das_client               ║")
message(STATUS "║               : das_admin                ║")
message(STATUS "╚══════════════════════════════════════════╝")
message(STATUS "")
