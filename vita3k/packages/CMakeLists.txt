add_library(packages STATIC
            src/archive_7z.cpp
            src/archive_read.cpp
            src/exfat.cpp
            src/license.cpp
            src/pkg.cpp
            src/pup.cpp
            src/sce_utils.cpp
            src/sfo.cpp
            include/packages/archive_7z.h
            include/packages/archive_read.h
            include/packages/exfat.h
            include/packages/license.h
            include/packages/functions.h
            include/packages/pkg.h
            include/packages/sce_types.h
            include/packages/sfo.h
)
target_include_directories(packages PUBLIC include)
target_link_libraries(packages PUBLIC emuenv util)
target_link_libraries(packages PRIVATE config crypto emuenv FAT16 io lzma-sdk miniz psvpfsparser vita-toolchain)
