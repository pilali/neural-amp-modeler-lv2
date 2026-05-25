if (MSVC)
	add_compile_options(
		"$<$<CONFIG:DEBUG>:/W4>"
		"$<$<CONFIG:RELEASE>:/O2>"
	)
else()
	add_compile_options(
		-Wall
		# -Wpedantic -Wextra -Wstrict-aliasing -Wunreachable-code -Weffc++ -Wno-unused-parameter
		"$<$<CONFIG:DEBUG>:-Og;-ggdb>"
	)
	# Release/RelWithDebInfo: prefer -Ofast for native builds, but in MOD_BUILD
	# the cross toolchain (mod-plugin-builder) controls optimization flags.
	if (NOT MOD_BUILD)
		add_compile_options(
			"$<$<CONFIG:RELWITHDEBINFO>:-Ofast>"
			"$<$<CONFIG:RELEASE>:-Ofast>"
		)
	endif()
endif()


if (CMAKE_SYSTEM_PROCESSOR MATCHES "(amd64)|(AMD64)|(x86_64)")
	option(USE_NATIVE_ARCH "Enable architecture-specific optimizations" OFF)

	if (USE_NATIVE_ARCH)
		if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
			add_compile_options(/arch:AVX2)
			message(STATUS "Enabling /arch:AVX2")

		else()
			add_compile_options(-march=x86-64-v3)
			message(STATUS "Enabling -march=x86-64-v3")
		endif()
	endif (USE_NATIVE_ARCH)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64)|(arm64)")
	# aarch64 always has NEON; Eigen will pick it up automatically.
	# Don't add -march here: mod-plugin-builder injects the right -mcpu/-mtune
	# (Cortex-A53 for moddwarf-new) via TARGET_CFLAGS/TARGET_CXXFLAGS.
	option(USE_NATIVE_ARCH "Enable architecture-specific optimizations" OFF)

	if (USE_NATIVE_ARCH AND NOT MOD_BUILD)
		add_compile_options(-mcpu=native)
		message(STATUS "Enabling -mcpu=native (aarch64)")
	endif()
endif ()

add_subdirectory(../deps/NeuralAudio NeuralAudio)

set(SOURCES nam_lv2.cpp
	nam_plugin.cpp
	nam_plugin.h)

set(NA_SOURCES ../deps/NeuralAudio/NeuralAudio/NeuralModel.h)

add_library(neural_amp_modeler SHARED ${SOURCES} ${NA_SOURCES})

target_include_directories(neural_amp_modeler PUBLIC ../deps/NeuralAudio)
target_include_directories(neural_amp_modeler PUBLIC ../deps/lv2/include)
target_include_directories(neural_amp_modeler PUBLIC ../deps/denormal)

target_link_libraries(neural_amp_modeler PRIVATE NeuralAudio)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})
source_group(NAM ${CMAKE_CURRENT_SOURCE_DIR} FILES ${NA_SOURCES})

option(DISABLE_DENORMALS "Disable floating point denormals" ON)

if (DISABLE_DENORMALS)
	add_definitions(-DDISABLE_DENORMALS)
endif (DISABLE_DENORMALS)

option(SMART_BYPASS_ENABLED "Enable auto-bypass on silence" OFF)

if (SMART_BYPASS_ENABLED)
	add_definitions(-DSMART_BYPASS_ENABLED)
endif (SMART_BYPASS_ENABLED)

set_target_properties(neural_amp_modeler
	PROPERTIES
	CXX_VISIBILITY_PRESET hidden
	PREFIX ""
)

# CMake-driven IPO doesn't play well with cross-toolchain LTO plugins,
# and mod-plugin-builder already injects -flto via its own optimization flags.
if (NOT MOD_BUILD)
	set_target_properties(neural_amp_modeler PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Platform

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_compile_definitions(neural_amp_modeler PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
