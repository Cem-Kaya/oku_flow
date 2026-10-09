cmake_minimum_required(VERSION 3.23)

project(OkuFlow HOMEPAGE_URL "https://okuflow.com" LANGUAGES CXX)

if(OKUFLOW_ENABLE_TESTS)
    enable_testing()
endif()

add_subdirectory(cmake)
