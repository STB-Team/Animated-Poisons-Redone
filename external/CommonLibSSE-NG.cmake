# CommonLibSSE-NG for SKSE plugins: one DLL for SE 1.5.97, AE 1.6.x and 1.7.x.
#
#   include(".../external/CommonLibSSE-NG.cmake")
#   target_link_libraries(<plugin> PRIVATE CommonLibSSE::CommonLibSSE)
#
# external/CommonLibSSE-NG is upstream CharmedBaryon/CommonLibSSE-NG as is (last commit 2024-09, it does not know 1.7).
# Our changes are external/patches/commonlibsse-ng-stb.patch, applied here at configure time if not applied yet:
#   - 1.6.x and anything newer (1.7.x) is AE; upstream took everything but 1.4 / 1.6 for SE and used SE ids on 1.7;
#   - Address Library format 5 (versionlib-1-7-*.bin, a flat RVA table indexed by id);
#   - an id missing from the table is an error on every runtime (upstream silently returned the next id's address);
#   - constants RUNTIME_SSE_1_6_1130/1170/1179/1_7_99/1_7_104, the typo in RUNTIME_SSE_1_6_1330.
# VR is off. (Skyrim-RE: details in docs/AE_1_7_99_Update.md.)

include_guard(GLOBAL)

set(STB_NG_DIR "${CMAKE_CURRENT_LIST_DIR}/CommonLibSSE-NG")
set(STB_NG_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/commonlibsse-ng-stb.patch")

if (NOT EXISTS "${STB_NG_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "CommonLibSSE-NG not found in ${STB_NG_DIR}: clone https://github.com/CharmedBaryon/CommonLibSSE-NG there "
	                    "(commit b93280e832f263dbef44e44cbe2936622a02f91a; in Skyrim-RE: git submodule update --init external/CommonLibSSE-NG)")
endif ()

find_package(Git REQUIRED)
execute_process(
	COMMAND "${GIT_EXECUTABLE}" -C "${STB_NG_DIR}" apply --reverse --check "${STB_NG_PATCH}"
	RESULT_VARIABLE STB_NG_PATCHED OUTPUT_QUIET ERROR_QUIET)
if (NOT STB_NG_PATCHED EQUAL 0)
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${STB_NG_DIR}" apply "${STB_NG_PATCH}"
		RESULT_VARIABLE STB_NG_APPLY ERROR_VARIABLE STB_NG_APPLY_ERR)
	if (NOT STB_NG_APPLY EQUAL 0)
		message(FATAL_ERROR "${STB_NG_PATCH} does not apply to ${STB_NG_DIR}:\n${STB_NG_APPLY_ERR}")
	endif ()
	message(STATUS "CommonLibSSE-NG: applied ${STB_NG_PATCH}")
endif ()

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
# rapidcsv is used by NG for VR only (version-*.csv); find_path does not search when the path is in the cache already
if (NOT RAPIDCSV_INCLUDE_DIRS)
	set(RAPIDCSV_INCLUDE_DIRS "${STB_NG_DIR}/include" CACHE PATH "rapidcsv: VR only, not used here" FORCE)
endif ()
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "Xbyak in the SKSE trampoline: a plugin that needs it sets ON before the include")

add_subdirectory("${STB_NG_DIR}" "${CMAKE_BINARY_DIR}/external/CommonLibSSE-NG")
