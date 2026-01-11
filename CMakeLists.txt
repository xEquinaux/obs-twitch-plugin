cmake_minimum_required(VERSION 3.16)
project(obs-twitch-chat VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# User: set OBS_DIR to point to your OBS Studio dev files if needed
# find_package(obs REQUIRED) # Optional: depends on your OBS build environment

add_library(obs-twitch-chat SHARED
    src/obs_twitch_chat.cpp
    src/chat_client.cpp
    src/text_renderer.cpp
)

target_include_directories(obs-twitch-chat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Find FreeType for text rendering
find_package(Freetype REQUIRED)
find_package(CURL REQUIRED)
if(FREETYPE_FOUND)
    message(STATUS "Found FreeType: ${FREETYPE_INCLUDE_DIRS}")
    target_include_directories(obs-twitch-chat PRIVATE ${FREETYPE_INCLUDE_DIRS})
    target_link_libraries(obs-twitch-chat PRIVATE ${FREETYPE_LIBRARIES})
endif()
if(CURL_FOUND)
    message(STATUS "Found CURL: ${CURL_LIBRARIES}")
    target_include_directories(obs-twitch-chat PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(obs-twitch-chat PRIVATE ${CURL_LIBRARIES})
endif()

# Link flags and OBS-specific linking will vary by environment. You may need to
# add OBS library dirs and link against obs, graphics, util, etc.
# Example (user may need to change):
# target_link_libraries(obs-twitch-chat PRIVATE obs)

install(TARGETS obs-twitch-chat LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins")
