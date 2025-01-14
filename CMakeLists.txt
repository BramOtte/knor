cmake_minimum_required(VERSION 3.14)

project(knor
    VERSION 1.0.0
    DESCRIPTION "Knor, a synthesis tool for HOA parity automata using BDDs"
    HOMEPAGE_URL "https://github.com/trolando/knor"
    LANGUAGES C CXX
)

# Dependencies
include(FetchContent)
FetchContent_Declare(
    lace
    GIT_REPOSITORY https://github.com/trolando/lace.git
    GIT_TAG        v1.4.2
    FIND_PACKAGE_ARGS
)
FetchContent_Declare(
    sylvan
    GIT_REPOSITORY https://github.com/trolando/sylvan.git
    GIT_TAG        v1.7.1
    FIND_PACKAGE_ARGS
)
FetchContent_Declare(
    oink
    GIT_REPOSITORY https://github.com/trolando/oink.git
    GIT_TAG        6f849c74bdaa3dbf44c808654f29864df45dcaa4
    FIND_PACKAGE_ARGS
)
FetchContent_Declare(
    abc
    GIT_REPOSITORY https://github.com/berkeley-abc/abc.git
    GIT_TAG        0ed81b34f1048a10aa1b6785c2fb65b526c77b5a
    FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(lace)
FetchContent_MakeAvailable(sylvan)
FetchContent_MakeAvailable(oink)
set (READLINE_FOUND FALSE)
add_definitions (-DABC_USE_STDINT_H)
FetchContent_MakeAvailable(abc)

# Parser generator
find_package(FLEX)
find_package(BISON)
FLEX_TARGET(hoalexer src/hoa.l ${CMAKE_CURRENT_BINARY_DIR}/hoalexer.c DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/hoalexer.h)
BISON_TARGET(hoaparser src/hoa.y ${CMAKE_CURRENT_BINARY_DIR}/hoaparser.c DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/hoaparser.h)
ADD_FLEX_BISON_DEPENDENCY(hoalexer hoaparser)

find_package(Boost REQUIRED random)

# Knor itself
add_executable(knor)
target_sources(knor
  PRIVATE
    src/aigcircuit.cpp
    src/aiger.c
    src/abcminimization.cpp
        src/aigencoder.cpp
    src/bddtools.cpp
    src/bisim.cpp
    src/knor.cpp
    src/simplehoa.c
    src/symgame.cpp
    ${BISON_hoaparser_OUTPUTS}
    ${FLEX_hoalexer_OUTPUTS}
)
target_compile_features(knor PUBLIC c_std_11 cxx_std_17)
target_compile_options(knor PRIVATE -Wall -Wextra -fno-strict-aliasing -Wno-deprecated -Wno-unused-parameter)
target_link_libraries(knor PRIVATE oink::oink sylvan::sylvan libabc)
target_link_libraries(knor PRIVATE Boost::boost)
if(CMAKE_HOST_SYSTEM MATCHES Linux)
  target_link_libraries(knor PRIVATE "-static")
endif()
target_include_directories(knor PRIVATE src)
