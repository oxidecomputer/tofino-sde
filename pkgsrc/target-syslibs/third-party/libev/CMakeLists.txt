project(libev VERSION 0.1 LANGUAGES C)

set(CMAKE_C_FLAGS "-g -O3 -w")

add_library(ev_o OBJECT ev.c ev.h ev++.h)
add_library(ev SHARED EXCLUDE_FROM_ALL $<TARGET_OBJECTS:ev_o>)

if(DEFINED LIBEV_EV_CHILD_ENABLE)
    target_compile_definitions(ev_o PRIVATE EV_CHILD_ENABLE=${LIBEV_EV_CHILD_ENABLE})
endif()
