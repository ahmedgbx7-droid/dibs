cmake_minimum_required(VERSION 3.14)
project(dibs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# مكتبة عشان نعمل سيرفر ويب
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

# مكتبة JSON
find_package(nlohmann_json REQUIRED)

add_executable(dibs src/main.cpp)
target_link_libraries(dibs PRIVATE httplib::httplib nlohmann_json::nlohmann_json)
