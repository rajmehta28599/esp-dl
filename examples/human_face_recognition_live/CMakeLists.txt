# Top-level project CMake for the live face detection + recognition demo
# Target board: Elecrow CrowPanel Advanced 7" ESP32-P4 HMI AI Display (1024x600)
cmake_minimum_required(VERSION 3.5)

add_compile_options(-fdiagnostics-color=always)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(human_face_recognition_live)
