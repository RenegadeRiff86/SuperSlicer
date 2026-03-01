# PrusaSlicer specific CMake

cmake_minimum_required(VERSION 3.10)
project(jpeg-compressor)

add_library(jpeg-compressor STATIC
    jpge.h
    jpge.cpp
)
