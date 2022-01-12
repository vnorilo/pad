project(pad CXX)
cmake_minimum_required(VERSION 3.2)

set(CPACK_PACKAGE_VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	add_definitions(
			-D_CRT_SECURE_NO_DEPREATE 
			-D_CRT_NONSTDC_NO_DEPRECATE 
			-D_CRT_SECURE_NO_WARNINGS
			-D_SCL_SECURE_NO_WARNINGS
			-DNOMINMAX
			-D_UNICODE -DUNICODE
			 "/wd4458" "/wd4457" "/wd4456")
else ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	add_definitions(-msse2 -fno-rtti -Wno-reorder -Wno-unused-function)
endif()

if (WIN32)
	set(PAD_AVAILABLE_HOSTAPIS asio wasapi)
elseif (APPLE)
	set(PAD_AVAILABLE_HOSTAPIS coreaudio)
endif()

include(FindJack.cmake)
if (JACK_FOUND)
	list(APPEND PAD_AVAILABLE_HOSTAPIS jack)
	if (JACK_INCLUDE_DIRS)
		include_directories("${JACK_INCLUDE_DIRS}")
	endif()
endif (JACK_FOUND )

if (PAD_AVAILABLE_HOSTAPIS)
	set(PAD_HOSTAPIS ${PAD_AVAILABLE_HOSTAPIS} CACHE STRING "Build PAD for a subset of asio;wasapi;coreaudio;jack")
endif (PAD_AVAILABLE_HOSTAPIS)

set(PAD_SOURCES HostAPI.cpp pad.cpp pad.h pad_channels.h HostAPI.h pad_samples.h pad_errors.h)

message(STATUS "Linking ${PAD_HOSTAPIS}")

MACRO(LIST_CONTAINS var value)
  SET(${var})
  FOREACH (value2 ${ARGN})
    IF (${value} STREQUAL ${value2})
      SET(${var} TRUE)
    ENDIF (${value} STREQUAL ${value2})
  ENDFOREACH (value2)
ENDMACRO(LIST_CONTAINS)

LIST_CONTAINS(contains asio ${PAD_HOSTAPIS})
if (contains)
    list(APPEND PAD_SOURCES AsioUtil.cpp pad_asio.cpp)	
    add_definitions(-DPAD_LINK_ASIO)
endif()

LIST_CONTAINS(contains wasapi ${PAD_HOSTAPIS})
if (contains)
	list(APPEND PAD_SOURCES pad_wasapi2.cpp cog/cog.h)
	add_definitions(-DPAD_LINK_WASAPI)
endif()

LIST_CONTAINS(contains coreaudio ${PAD_HOSTAPIS})
if (contains)
    list(APPEND PAD_SOURCES pad_coreaudio.cpp)
    add_definitions(-DPAD_LINK_COREAUDIO)
endif()

LIST_CONTAINS(contains jack ${PAD_HOSTAPIS})
if (contains)
	if (NOT JACK_FOUND)
		message(FATAL_ERROR "FindJack failed: please specify JACK_LIBRARY and JACK_INCLUDE_DIR")
	endif (NOT JACK_FOUND)
	list(APPEND PAD_SOURCES pad_jack.cpp)	
	include_directories(${JACK_INCLUDE_DIR})
	add_definitions(-DPAD_LINK_JACK)
endif()

add_library(pad STATIC ${PAD_SOURCES})

LIST_CONTAINS(contains jack ${PAD_HOSTAPIS})
if (contains)
	target_link_libraries( pad ${JACK_LIBRARY} )
endif()

LIST_CONTAINS(contains coreaudio ${PAD_HOSTAPIS})
if (contains)
	find_library(AUDIOUNIT_FRAMEWORK AudioUnit)
	find_library(COREAUDIO_FRAMEWORK CoreAudio)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    find_library(SECURITY_FRAMEWORK Security)
    set(FRAMEWORKS ${AUDIOUNIT_FRAMEWORK} ${COREAUDIO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK} ${SECURITY_FRAMEWORK})
	message(STATUS "Using ${FRAMEWORKS}")
	target_link_libraries( pad ${FRAMEWORKS} )
endif()

set_target_properties( pad 
		       PROPERTIES 
		       PUBLIC_HEADER "pad.h;pad_errors.h")

set(PAD_TARGET_LIBRARY_ONLY ON CACHE BOOL "Do not build test driver, export or install targets")

if (NOT PAD_LIBRARY_TARGET_ONLY)
add_executable(pad_test "test1.cpp")
target_link_libraries( pad_test pad )

target_include_directories(pad INTERFACE 
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
	$<INSTALL_INTERFACE:include>)

install( TARGETS pad EXPORT PADTargets
	LIBRARY DESTINATION lib 
	ARCHIVE DESTINATION lib 
	PUBLIC_HEADER DESTINATION include 
    FRAMEWORK DESTINATION "/Library/Frameworks" )

include(CMakePackageConfigHelpers)
write_basic_package_Version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/pad/PADConfigVersion.cmake"
	VERSION "${CPACK_PACKAGE_VERSION}"
	COMPATIBILITY AnyNewerVersion
)

export(EXPORT PADTargets
	FILE "${CMAKE_CURRENT_BINARY_DIR}/pad/PADTargets.cmake"
	NAMESPACE PAD::)

configure_file(PADConfig.cmake
	"${CMAKE_CURRENT_BINARY_DIR}/pad/PADConfig.cmake"
	COPYONLY)

set(ConfigPackageLocation share/pad/cmake)
install(EXPORT PADTargets
		FILE PADTargets.cmake
		NAMESPACE PAD::
		DESTINATION ${ConfigPackageLocation})

install(FILES
	PADConfig.cmake
	"${CMAKE_CURRENT_BINARY_DIR}/pad/PADConfigVersion.cmake"
	DESTINATION ${ConfigPackageLocation})

export(EXPORT PADTargets FILE pad-config.cmake NAMESPACE PAD::)
export(PACKAGE pad)
endif()
