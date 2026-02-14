cmake_minimum_required(VERSION 3.20)
project(ManasPDF VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build PDFCore native library
add_subdirectory(src/PDFCore)
