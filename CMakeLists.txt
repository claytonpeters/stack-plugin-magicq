cmake_minimum_required(VERSION 3.12)
project(stack-plugin-magicq)
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-ggdb")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3 -Wall")
list(APPEND CMAKE_CXX_FLAGS "-std=c++17 -Werror=return-type")

# Add in program for compiling resources
find_program(GLIB_COMPILE_RESOURCES NAMES glib-compile-resources REQUIRED)

# Compile resource target: Stack
add_custom_command(
	OUTPUT src/resources.c
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	COMMAND ${GLIB_COMPILE_RESOURCES}
	ARGS --generate-source --target=src/resources.c src/resources.xml
	VERBATIM
	MAIN_DEPENDENCY src/resources.xml
	DEPENDS ui/StackMagicQCue.ui icons/stackmagicqcue.png
)
add_custom_target(stackmagicqcue-resources-target DEPENDS src/resources.c)
set_source_files_properties(src/resources.c PROPERTIES GENERATED TRUE)

add_library(StackMagicQCue SHARED src/StackMagicQCue.cpp src/resources.c)
add_dependencies(StackMagicQCue stackmagicqcue-resources-target)
include(FindPkgConfig)
include(FindPackageHandleStandardArgs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
find_package(Threads REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

## Required libraries
include_directories("${PROJECT_SOURCE_DIR}")
include_directories("${PROJECT_SOURCE_DIR}/../stack/src")
include_directories(${GTK3_INCLUDE_DIRS})
include_directories(${JSONCPP_INCLUDE_DIRS})
link_directories(${GTK3_LIBRARY_DIRS})
link_directories(${JSONCPP_LIBRARY_DIRS})
add_definitions(${GTK3_CFLAGS_OTHER})
