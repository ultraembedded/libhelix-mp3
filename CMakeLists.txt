cmake_minimum_required(VERSION 3.9)

project(helixmp3)

FILE(GLOB HelixMP3_real_src
        real/*.c)

include_directories(pub)

add_library(${PROJECT_NAME}
        ${HelixMP3_real_src}
        mp3dec.c
        mp3tabs.c)