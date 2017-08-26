cmake_minimum_required(VERSION 3.5)
project(pr_ec)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -std=gnu99 -Wno-format-security -Werror -fsanitize=address -fno-omit-frame-pointer")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -std=gnu99 -Wno-format-security -fsanitize=address -fno-omit-frame-pointer")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -std=gnu99 -Wno-format-security -fno-omit-frame-pointer")

set(SOURCE_FILES_GFC
        ./gfclient_download.c
        ./gfclient.c
        ./workload.c
        ./steque.c)
add_executable(gfclient_download ${SOURCE_FILES_GFC})
target_include_directories(gfclient_download PRIVATE .)
target_link_libraries(gfclient_download pthread rt)

set(SOURCE_FILES_PROXY
        ./gfserver.c
        ./handlers.c
        ./shm_channel.c
        ./steque.c
        ./webproxy.c)
add_executable(webproxy ${SOURCE_FILES_PROXY})
target_include_directories(webproxy PRIVATE .)
target_link_libraries(webproxy pthread rt curl)

set(SOURCE_FILES_SC
        ./simplecache.c
        ./shm_channel.c
        ./steque.c
        ./simplecached.c)
add_executable(simplecached ${SOURCE_FILES_SC})
target_include_directories(simplecached PRIVATE .)
target_link_libraries(simplecached pthread rt)


