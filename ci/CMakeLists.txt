cmake_minimum_required(VERSION 3.16)
project(obs_twitch_chat_ci CXX)
set(CMAKE_CXX_STANDARD 17)

# Use FreeType for text rendering in CI
find_package(Freetype REQUIRED)

add_executable(obs_twitch_chat_cli
    ../src/cli_main.cpp
    ../src/chat_client.cpp
    ../src/text_renderer.cpp
)

target_include_directories(obs_twitch_chat_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${FREETYPE_INCLUDE_DIRS}
)

target_link_libraries(obs_twitch_chat_cli PRIVATE ${FREETYPE_LIBRARIES} ws2_32)
