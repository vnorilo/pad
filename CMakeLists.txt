cmake_minimum_required(VERSION 3.1)
project(pad)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	add_definitions(
			-D_CRT_SECURE_NO_DEPREATE 
			-D_CRT_NONSTDC_NO_DEPRECATE 
			-D_CRT_SECURE_NO_WARNINGS
			-D_SCL_SECURE_NO_WARNINGS
			-D_UNICODE -DUNICODE
			 "/wd4458" "/wd4457" "/wd4456")
else ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
endif()

include(FindJack.cmake)

if (WIN32)
	set(PAD_AVAILABLE_HOSTAPIS asio wasapi STRING "Subset of asio;wasapi;coreaudio;jack")
elseif (APPLE)
	set(PAD_AVAILABLE_HOSTAPIS coreaudio STRING "Subset of asio;wasapi;coreaudio;jack")
else (APPLE)
	set(PAD_AVAILABLE_HOSTAPIS jack STRING "Subset of asio;wasapi;coreaudio;jack")
endif()

if (JACK_FOUND)
	list(APPEND PAD_AVAILABLE_HOSTAPIS jack)
endif (JACK_FOUND )

set(PAD_HOSTAPIS CACHE STRING ${PAD_AVAILABLE_HOSTAPIS})

set(PAD_SOURCES HostAPI.cpp PAD.cpp)

message(STATUS "Building for ${PAD_HOSTAPIS}")

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
endif()

LIST_CONTAINS(contains wasapi ${PAD_HOSTAPIS})
if (contains)
	list(APPEND PAD_SOURCES pad_wasapi.cpp)
endif()

LIST_CONTAINS(contains coreaudio ${PAD_HOSTAPIS})
if (contains)
	list(APPEND PAD_SOURCES pad_coreaudio.cpp)
endif()

LIST_CONTAINS(contains jack ${PAD_HOSTAPIS})
if (contains)
	if (NOT JACK_FOUND)
		message(FATAL_ERROR "FindJack failed: please specify JACK_LIBRARY and JACK_INCLUDE_DIR")
	endif (NOT JACK_FOUND)
	list(APPEND PAD_SOURCES pad_jack.cpp)	
	include_directories(${JACK_INCLUDE_DIR})
endif()

add_library(pad STATIC ${PAD_SOURCES})

LIST_CONTAINS(contains jack ${PAD_HOSTAPIS})
if (contains)
	target_link_libraries( pad ${JACK_LIBRARY} )
endif()

add_executable(pad_test test1.cpp)
target_link_libraries( pad_test pad )
