# Discover the projects that use CMake in the subdirectories.
# Note that explicit cmake invocation is required every time a new project is
# added or removed.
file(GLOB entries *)
foreach(entry ${entries})
  if(IS_DIRECTORY ${entry} AND EXISTS ${entry}/CMakeLists.txt)
    if((NOT ${entry} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR}/compiler-rt) AND
       (NOT ${entry} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR}/dragonegg) AND
       (NOT ${entry} STREQUAL ${CMAKE_CURRENT_SOURCE_DIR}/libcxx))
      add_subdirectory(${entry})
    endif()
  endif()
endforeach(entry)

# Also add in libc++ and compiler-rt trees if present (and we have
# a sufficiently recent version of CMake where required).
if(${LLVM_BUILD_RUNTIME})
  # MSVC isn't quite working with libc++ yet, disable it until issues are
  # fixed.
  if(NOT MSVC)
    add_llvm_external_project(libcxx)
  endif()
  if(NOT LLVM_BUILD_EXTERNAL_COMPILER_RT)
    add_llvm_external_project(compiler-rt)
  endif()
endif()

add_llvm_external_project(dragonegg)
