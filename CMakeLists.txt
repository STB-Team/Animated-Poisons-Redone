cmake_minimum_required(VERSION 3.21)

# ---- Cache build vars ----

macro(set_from_environment VARIABLE)
	if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
		set(${VARIABLE} $ENV{${VARIABLE}})
	endif ()
endmacro()

set_from_environment(VCPKG_ROOT)
if (NOT DEFINED VCPKG_ROOT)
	message(FATAL_ERROR "Variable VCPKG_ROOT is not set.")
endif ()

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(FATAL_ERROR "In-source builds not allowed.")
endif()

set(Boost_USE_STATIC_RUNTIME OFF CACHE BOOL "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

# ---- Project ----

project(
	AnimatedPoisonsRedone
	VERSION 4.7.3
	LANGUAGES CXX
)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/include/Version.h @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)

if (MSVC)
	add_compile_definitions(_UNICODE)
	if (NOT ${CMAKE_GENERATOR} STREQUAL "Ninja")
		add_compile_options(/MP)
	endif ()
endif ()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(Boost_USE_STATIC_LIBS ON)

# ---- Dependencies ----

# CommonLibSSE-NG (external/CommonLibSSE-NG + our patch for 1.7.x): one DLL for SE 1.5.97, AE 1.6.x and 1.7.x --
# addresses through RELOCATION_ID, the runtime is detected at load.
# In the source package it sits next to this file (external/), in the Skyrim-RE repository two levels up.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/CommonLibSSE-NG.cmake")
	include("${CMAKE_CURRENT_SOURCE_DIR}/external/CommonLibSSE-NG.cmake")
else ()
	include("${CMAKE_CURRENT_SOURCE_DIR}/../../external/CommonLibSSE-NG.cmake")
endif ()

find_package(spdlog REQUIRED CONFIG)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")

# ---- Sources ----

set(SOURCES
	src/PCH.h
	src/Settings.h
	src/Settings.cpp
	src/Flow.h
	src/Flow.cpp
	src/Placement.h
	src/Placement.cpp
	src/BuiltinTransforms.inl
	src/HandItems.h
	src/HandItems.cpp
	src/Poison.h
	src/Poison.cpp
	src/Hooks.h
	src/Hooks.cpp
	src/Diagnostics.h
	src/Diagnostics.cpp
	src/main.cpp
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})

add_library(
	${PROJECT_NAME}
	SHARED
	${SOURCES}
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	.clang-format
	.editorconfig
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/src
		${SIMPLEINI_INCLUDE_DIRS}
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		CommonLibSSE::CommonLibSSE
		spdlog::spdlog
		nlohmann_json::nlohmann_json
)

if (MSVC)
	target_compile_options(
		${PROJECT_NAME}
		PRIVATE
			/sdl /utf-8 /Zi /permissive-
			/Zc:alignedNew /Zc:auto /Zc:char8_t /Zc:__cplusplus /Zc:externC /Zc:externConstexpr /Zc:forScope
			/Zc:hiddenFriend /Zc:implicitNoexcept /Zc:lambda /Zc:noexceptTypes /Zc:preprocessor
			/Zc:referenceBinding /Zc:rvalueCast /Zc:sizedDealloc /Zc:strictStrings /Zc:ternary
			/Zc:threadSafeInit /Zc:tlsGuards /Zc:trigraphs /Zc:wchar_t
			/external:anglebrackets /external:W0
			/W4 /WX
			"$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
	)
	target_link_options(
		${PROJECT_NAME}
		PRIVATE
			/WX
			"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
			"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
endif ()

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

# ---- Deploy ----
# Output: build/Release/AnimatedPoisonsRedone.dll (+ data/SKSE/Plugins/AnimatedPoisonsRedone.ini)
