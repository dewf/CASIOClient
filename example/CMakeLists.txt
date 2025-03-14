cmake_minimum_required(VERSION 3.30)
project(example)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(../library ${CMAKE_CURRENT_BINARY_DIR}/LibraryBuild)

add_executable(example main.cpp)

target_link_libraries(example PRIVATE CASIOClient)

#add_custom_command(TARGET example POST_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy
#        $<TARGET_FILE:CASIOClient>  # Gets the built DLL path
#        $<TARGET_FILE_DIR:example>  # Destination: same as Executable
#        COMMENT "Copying Library DLL to Executable directory"
#)

add_custom_target(CopyLibraryDLL ALL
        COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:CASIOClient>
        $<TARGET_FILE_DIR:example>
        COMMENT "Copying updated Library DLL to Executable directory"
        DEPENDS CASIOClient  # Ensures it runs whenever Library is updated
)

add_dependencies(example CopyLibraryDLL)
