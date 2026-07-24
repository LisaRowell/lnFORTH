cmake_minimum_required(VERSION 3.16)

project(lnFORTH LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mkDictionary util/mkDictionary.cpp
                            lnFORTH.h
                            Platform.h)
add_custom_command(OUTPUT Dictionary.cpp Dictionary.h
                   COMMAND mkDictionary
                   DEPENDS mkDictionary)
add_executable(lnFORTH linux/main.cpp
                       lnFORTH.h
                       VirtualMachine.h
                       VirtualMachine.cpp
                       Dictionary.h
                       Dictionary.cpp
                       Platform.h
                       linux/Linux.cpp)
add_executable(mkdisk util/mkdisk.cpp)

include(GNUInstallDirs)
install(TARGETS lnFORTH
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
