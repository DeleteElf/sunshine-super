# windows specific target definitions
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
#        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib)
