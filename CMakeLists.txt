cmake_minimum_required(VERSION 3.16)

project(libcheckpoint C ASM)

include(GNUInstallDirs)
include(cmake/Options.cmake)
include(cmake/TargetArchitecture.cmake)
include(cmake/DiftLayout.cmake)
include(cmake/RuntimeTargets.cmake)
include(cmake/DiftWrappers.cmake)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
