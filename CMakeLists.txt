cmake_minimum_required(VERSION 3.16)

project(lnFORTH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lnFORTH lnFORTH.cpp
                       linux/Linux.cpp
                       Platform.h
                       linux/main.cpp
                       lnFORTH.h)
add_executable(mkdisk util/mkdisk.cpp)

include(GNUInstallDirs)
install(TARGETS lnFORTH
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
