# bmoil2sharesight
BmoIL2ShareSight is a small command-line tool to convert Investorline CSV files to a format ShareSight can understand. Handles multi-leg trades properly.

## Introduction
Describe how to run it here.

## Building
It's a CMake C++ project. Install CMake and a C++20 compiler (MSVC on Windows,
or Clang/GCC on Linux). The Tacent library is fetched automatically at configure
time, or discovered if it is already installed on the machine.

Windows:
* `mkdir build`
* `cd build`
* `cmake ..`
* `cmake --build . --config Release`

Linux:
* `mkdir build && cd build`
* `cmake ..`
* `make`

## Running
Run the `bmoil2sharesight` executable from a command prompt or shell. It prints
its name and version number:
* `BmoIL2ShareSight V0.1.0`

## Version
The version number is defined in a single place, `Src/Version.cmake.h`. That one
file drives both the CMake project version and the value the tool prints at
runtime, so changing the number there updates both.
