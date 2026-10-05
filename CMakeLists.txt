# Should really specify a range of tested versions.
cmake_minimum_required(VERSION 3.16)

cmake_policy(SET CMP0091 NEW)

# Set default build-type (AKA the configuration in other IDEs).
set(CMAKE_BUILD_TYPE_INIT Release)

# Setup Release and Debug build-types (only).
# No reason to set CMAKE_CONFIGURATION_TYPES if it's not a multiconfig generator
# Also no reason mess with CMAKE_BUILD_TYPE if it's a multiconfig generator.
get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (isMultiConfig)
	set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
else()
	if (NOT DEFINED CMAKE_BUILD_TYPE)
		message(STATUS "Bmoil2ShareSight -- Default to Release build.")
		set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose Build Type" FORCE)
	endif()
	message(STATUS "Bmoil2ShareSight -- Build type set to: ${CMAKE_BUILD_TYPE}")
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY HELPSTRING "Choose Build Type")

	# Set the valid options for cmake-gui drop-down list.
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

# This include sets the version string variable (BMOIL2SHARESIGHT_VERSION) used below and at runtime.
include("Src/Version.cmake.h")
project(bmoil2sharesight VERSION "${BMOIL2SHARESIGHT_VERSION}" LANGUAGES C CXX)
message(STATUS "Bmoil2ShareSight -- ProjectName ${PROJECT_NAME} ProjectVersion ${PROJECT_VERSION}")
message(STATUS "Bmoil2ShareSight -- Major:${PROJECT_VERSION_MAJOR} Minor:${PROJECT_VERSION_MINOR} Revision:${PROJECT_VERSION_PATCH}")

# Find Git module.
find_package(Git)
if (Git_FOUND)
	message(STATUS "Bmoil2ShareSight -- Git found: ${GIT_EXECUTABLE}")
endif()

# We want a better default for install prefix. It is bad form to be modifying system files from a cmake build of anything.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}" CACHE PATH "..." FORCE)
endif()
message(STATUS "Bmoil2ShareSight -- InstallPrefix ${CMAKE_INSTALL_PREFIX}")

# Grab the Tacent library. Prefer a locally installed Tacent (via find_package) if one is present,
# otherwise fetch it from github at configure time.
include(FetchContent)
FetchContent_Declare(
	tacent
	GIT_REPOSITORY https://github.com/bluescan/tacent.git
	GIT_TAG master
	# GIT_TAG v0.8.19
	FIND_PACKAGE_ARGS NAMES tacent
)
FetchContent_MakeAvailable(tacent)

# Files needed to create executable.
add_executable(
	${PROJECT_NAME}
	Src/BmoIL2ShareSight.cpp
	Src/Version.cmake.h
)

# Include directories needed to build.
target_include_directories(
	"${PROJECT_NAME}"
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/Src
		$<TARGET_PROPERTY:Foundation,INTERFACE_INCLUDE_DIRECTORIES>
		$<TARGET_PROPERTY:Math,INTERFACE_INCLUDE_DIRECTORIES>
		$<TARGET_PROPERTY:System,INTERFACE_INCLUDE_DIRECTORIES>
)

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
		ARCHITECTURE_X64
		$<$<CONFIG:Debug>:CONFIG_DEBUG>
		$<$<CONFIG:Release>:CONFIG_RELEASE>
		$<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_DEPRECATE _LIB>
		$<$<PLATFORM_ID:Windows>:PLATFORM_WINDOWS>
		$<$<PLATFORM_ID:Linux>:PLATFORM_LINUX>
)

# Set compiler option flags based on specific compiler and configuration.
target_compile_options(
	${PROJECT_NAME}
	PRIVATE
		# MSVC compiler. Warning /utf-8 is needed for MSVC to support UTF-8 source files.
		$<$<CXX_COMPILER_ID:MSVC>:/W2 /GS /Gy /Zc:wchar_t /Gm- /Zc:inline /fp:precise /WX- /Zc:forScope /Gd /FC /utf-8>
		$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:MSVC>>:/Od>
		$<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2>

		# Clang compiler.

		# GNU compiler.
		$<$<CXX_COMPILER_ID:GNU>:-Wno-unused-result>

		# Clang and GNU.
		$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wno-multichar>
		$<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wno-switch>
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

# This is how you set things like CMAKE_DEBUG_POSTFIX for a target.
set_target_properties(
	${PROJECT_NAME}
	PROPERTIES
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# Dependencies.
target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		Foundation Math System

		# These are unnecessary if using VSCode/CMake Tools, but are needed when building from bash on Linux.
		$<$<PLATFORM_ID:Linux>:m>
		$<$<PLATFORM_ID:Linux>:stdc++>
		$<$<PLATFORM_ID:Linux>:stdc++fs>
		$<$<PLATFORM_ID:Windows>:shlwapi.lib>
)

if (MSVC)
	if (CMAKE_BUILD_TYPE MATCHES Debug)
		target_link_options(${PROJECT_NAME} PRIVATE "/NODEFAULTLIB:LIBCMT.lib")
	endif()
endif()

# Installation.
install(
	TARGETS ${PROJECT_NAME}
	RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/Bmoil2ShareSightInstall"
)
