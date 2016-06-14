project(pad CXX)
cmake_minimum_required(VERSION 3.2)

set(CPACK_PACKAGE_VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	message(STATUS "Setting up MSVC compiler flags...")
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

if(APPLE)
	set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build architecture")
endif()

if(CMAKE_GENERATOR STREQUAL Xcode)
    set(CMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++11")
    set(CMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++")	
endif()

if (WIN32)
	set(PAD_AVAILABLE_HOSTAPIS asio wasapi)
elseif (APPLE)
	set(PAD_AVAILABLE_HOSTAPIS coreaudio)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "10.8" CACHE STRING "OS X Deployment target")
	set(CMAKE_OSX_BUILD_ARCHITECTURE "x86_64" CACHE STRING "Build architecture")
else (APPLE)
	set(PAD_AVAILABLE_HOSTAPIS jack)
endif()

include(FindJack.cmake)
if (JACK_FOUND)
	list(APPEND PAD_AVAILABLE_HOSTAPIS jack)
endif (JACK_FOUND )

if (PAD_AVAILABLE_HOSTAPIS)
	set(PAD_HOSTAPIS ${PAD_AVAILABLE_HOSTAPIS} CACHE STRING "Build PAD for a subset of asio;wasapi;coreaudio;jack")
endif (PAD_AVAILABLE_HOSTAPIS)

set(PAD_SOURCES HostAPI.cpp PAD.cpp)

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
	list(APPEND PAD_SOURCES pad_wasapi.cpp)
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
	message(STATUS "Using ${AUDIOUNIT_FRAMEWORK} ${COREAUDIO_FRAMEWORK}")
	target_link_libraries( pad ${AUDIOUNIT_FRAMEWORK} ${COREAUDIO_FRAMEWORK})
endif()

set_target_properties( pad 
		       PROPERTIES 
		       PUBLIC_HEADER "PAD.h;PADErrors.h")

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