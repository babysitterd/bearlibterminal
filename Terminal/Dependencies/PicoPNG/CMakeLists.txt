cmake_minimum_required(VERSION 3.7)

project(picopng)

set(SOURCES ./Source/PicoPNG.cpp)

include_directories(./Include)

if (UNIX)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC") 
endif()

add_library(picopng STATIC ${SOURCES})
