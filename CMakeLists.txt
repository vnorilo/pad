project(pad CXX)
cmake_minimum_required(VERSION 2.8)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	add_definitions(
			-D_CRT_SECURE_NO_DEPREATE 
			-D_CRT_NONSTDC_NO_DEPRECATE 
			-D_CRT_SECURE_NO_WARNINGS
			-D_SCL_SECURE_NO_WARNINGS
			-D_UNICODE -DUNICODE
			 "/wd4458" "/wd4457" "/wd4456")
	message(STATUS "Setting up MSVC compiler flags...")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	add_definitions(-std=c++11 -msse2)
	message(STATUS "Setting up Gnu compiler flags...")	
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
	add_definitions(-std=c++11 -stdlib=libc++)
	message(STATUS "Setting up AppleClang compiler flags...")
else ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
message(FATAL_ERROR "Unknown C++ compiler ${CMAKE_CXX_COMPILER_ID}")
endif()

if (WIN32)
	set(PAD_AVAILABLE_HOSTAPIS asio wasapi)
elseif (APPLE)
	set(PAD_AVAILABLE_HOSTAPIS coreaudio)
	set(CMAKE_OSX_DEPLOYMENT_TARGET "10.8" CACHE STRING "Support OS X Version")
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

add_executable(pad_test test1.cpp)
target_link_libraries( pad_test pad )

