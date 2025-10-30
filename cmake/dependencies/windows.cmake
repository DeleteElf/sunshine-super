# windows specific dependencies
set(MINHOOK_INCLUDE_DIR "D:/msys64/mingw64/include")

# Make sure MinHook is installed

find_library(MINHOOK_LIBRARY libMinHook.a REQUIRED PATHS "D:/msys64/mingw64/lib")
find_path(MINHOOK_INCLUDE_DIR MinHook.h PATH_SUFFIXES include REQUIRED)

add_library(minhook::minhook STATIC IMPORTED)
set_property(TARGET minhook::minhook PROPERTY IMPORTED_LOCATION ${MINHOOK_LIBRARY})
target_include_directories(minhook::minhook INTERFACE ${MINHOOK_INCLUDE_DIR})
