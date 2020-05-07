# This strings autochanged from release_lib.sh:
set(VERSION_REVISION 54433)
set(VERSION_MAJOR 20)
set(VERSION_MINOR 3)
set(VERSION_PATCH 9)
set(VERSION_GITHASH 706a71eeba564d410c3f80d5f23d2f98e61cb8e8)
set(VERSION_DESCRIBE v20.3.9.1-lts)
set(VERSION_STRING 20.3.9.1)
# end of autochange

set(VERSION_EXTRA "" CACHE STRING "")
set(VERSION_TWEAK "" CACHE STRING "")

if (VERSION_TWEAK)
    string(CONCAT VERSION_STRING ${VERSION_STRING} "." ${VERSION_TWEAK})
endif ()

if (VERSION_EXTRA)
    string(CONCAT VERSION_STRING ${VERSION_STRING} "." ${VERSION_EXTRA})
endif ()

set (VERSION_NAME "${PROJECT_NAME}")
set (VERSION_FULL "${VERSION_NAME} ${VERSION_STRING}")
set (VERSION_SO "${VERSION_STRING}")

math (EXPR VERSION_INTEGER "${VERSION_PATCH} + ${VERSION_MINOR}*1000 + ${VERSION_MAJOR}*1000000")

if(YANDEX_OFFICIAL_BUILD)
    set(VERSION_OFFICIAL " (official build)")
endif()