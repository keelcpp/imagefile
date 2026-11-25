cmake_minimum_required(VERSION 3.20)
project(keel_imagefile VERSION 0.1.0 LANGUAGES CXX)

option(KEEL_IMAGEFILE_NO_EXCEPTIONS_RTTI "Compile keel_imagefile without C++ exceptions and RTTI" OFF)
option(KEEL_IMAGEFILE_BUILD_TESTS "Build keel_imagefile unit tests and header purity checks" ${PROJECT_IS_TOP_LEVEL})
option(KEEL_IMAGEFILE_UNITY_BUILD "Enable unity build on keel_imagefile" OFF)

if(NOT TARGET keel::keel)
    find_package(keel QUIET CONFIG)
endif()
if(NOT TARGET keel::keel)
    include(FetchContent)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../keel/CMakeLists.txt" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_KEEL)
        set(FETCHCONTENT_SOURCE_DIR_KEEL "${CMAKE_CURRENT_SOURCE_DIR}/../keel" CACHE PATH "Local keel checkout")
    endif()
    if(KEEL_IMAGEFILE_NO_EXCEPTIONS_RTTI)
        set(KEEL_NO_EXCEPTIONS_RTTI ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        keel
        GIT_REPOSITORY https://github.com/keelcpp/keel.git
        GIT_TAG        869a5c07a5d77341e5f6acbac5f8e024b274ea08
    )
    FetchContent_MakeAvailable(keel)
endif()

if(NOT TARGET keel::compress)
    find_package(keel_compress QUIET CONFIG)
    if(TARGET keel::keel_compress AND NOT TARGET keel::compress)
        add_library(keel::compress ALIAS keel::keel_compress)
    endif()
endif()
if(NOT TARGET keel::compress)
    include(FetchContent)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../compress/CMakeLists.txt" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_KEEL_COMPRESS)
        set(FETCHCONTENT_SOURCE_DIR_KEEL_COMPRESS "${CMAKE_CURRENT_SOURCE_DIR}/../compress" CACHE PATH "Local keel_compress checkout")
    endif()
    if(KEEL_IMAGEFILE_NO_EXCEPTIONS_RTTI)
        set(KEEL_COMPRESS_NO_EXCEPTIONS_RTTI ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        keel_compress
        GIT_REPOSITORY https://github.com/keelcpp/compress.git
        GIT_TAG        c2d586b3bb7f8c874c1fb0ecb0205b42c7ef2422
    )
    FetchContent_MakeAvailable(keel_compress)
endif()

add_library(keel_imagefile STATIC
    src/image.cpp
    src/png.cpp
    src/jpeg.cpp
    src/tiff.cpp
    src/bmp.cpp
    src/tga.cpp
    src/pnm.cpp
    src/gif.cpp
)
add_library(keel::imagefile ALIAS keel_imagefile)

target_link_libraries(keel_imagefile
    PUBLIC  keel::keel
    PRIVATE keel::compress
)
target_compile_features(keel_imagefile PUBLIC cxx_std_20)
set_target_properties(keel_imagefile PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(KEEL_IMAGEFILE_UNITY_BUILD)
    set_target_properties(keel_imagefile PROPERTIES UNITY_BUILD ON)
endif()

target_include_directories(keel_imagefile PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(keel_imagefile PRIVATE /W4 /utf-8)
    if(KEEL_IMAGEFILE_NO_EXCEPTIONS_RTTI)
        string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        target_compile_options(keel_imagefile PUBLIC /EHs-c- /GR-)
        target_compile_definitions(keel_imagefile PUBLIC _HAS_EXCEPTIONS=0)
    endif()
else()
    target_compile_options(keel_imagefile PRIVATE -Wall -Wextra -Wpedantic)
    if(KEEL_IMAGEFILE_NO_EXCEPTIONS_RTTI)
        target_compile_options(keel_imagefile PUBLIC -fno-exceptions -fno-rtti)
    endif()
endif()

if(COMMAND keel_enforce_guidelines)
    keel_enforce_guidelines(keel_imagefile
        SCOPE ALL
        BAN_RAW_INTS ALL
        BAN_RAW_FLOATS ALL
        HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
        STANDALONE_HEADERS ${KEEL_IMAGEFILE_BUILD_TESTS}
        TEST_NAME test_guidelines_headers
        WARNINGS OFF
    )
endif()

include(CMakePackageConfigHelpers)
install(TARGETS keel_imagefile EXPORT keel_imagefileTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/keel DESTINATION include)
install(EXPORT keel_imagefileTargets
    FILE keel_imagefileTargets.cmake
    NAMESPACE keel::
    DESTINATION lib/cmake/keel_imagefile
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/keel_imagefileConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/keel_imagefileConfig.cmake"
    INSTALL_DESTINATION lib/cmake/keel_imagefile
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/keel_imagefileConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/keel_imagefileConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/keel_imagefileConfigVersion.cmake"
    DESTINATION lib/cmake/keel_imagefile
)

if(KEEL_IMAGEFILE_BUILD_TESTS)
    enable_testing()

    add_executable(test_imagefile tests/test_imagefile.cpp)
    target_link_libraries(test_imagefile PRIVATE keel::imagefile)
    target_compile_definitions(test_imagefile PRIVATE
        KEEL_IMAGEFILE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    if(MSVC)
        target_compile_options(test_imagefile PRIVATE /W4 /utf-8)
    else()
        target_compile_options(test_imagefile PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(COMMAND keel_enforce_guidelines)
        keel_enforce_guidelines(test_imagefile
            SCOPE ALL
            BAN_RAW_INTS ALL
            BAN_RAW_FLOATS ALL
            WARNINGS OFF
        )
    endif()
    add_test(NAME test_imagefile COMMAND test_imagefile)

    get_target_property(KEEL_CORE_INC_DIRS keel::keel INTERFACE_INCLUDE_DIRECTORIES)
    set(KEEL_CORE_INC_CLEAN "")
    foreach(IDIR IN LISTS KEEL_CORE_INC_DIRS)
        string(REGEX REPLACE "\\$<BUILD_INTERFACE:([^>]+)>" "\\1" IDIR_STRIPPED "${IDIR}")
        if(NOT IDIR_STRIPPED MATCHES "\\$<INSTALL_INTERFACE:" AND EXISTS "${IDIR_STRIPPED}")
            list(APPEND KEEL_CORE_INC_CLEAN "${IDIR_STRIPPED}")
        endif()
    endforeach()
    string(REPLACE ";" "|" KEEL_DEP_INC_JOINED "${KEEL_CORE_INC_CLEAN}")
    string(REPLACE ";" "|" KEEL_STD_INC_JOINED "${CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES}")

    add_test(
        NAME test_header_purity
        COMMAND ${CMAKE_COMMAND}
            "-DKEEL_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include"
            "-DKEEL_DEP_INC_DIRS=${KEEL_DEP_INC_JOINED}"
            "-DCOMPILER_PATH=${CMAKE_CXX_COMPILER}"
            "-DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
            "-DSTD_INC_DIRS=${KEEL_STD_INC_JOINED}"
            "-DCMAKE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckHeaderPurity.cmake"
    )
endif()
