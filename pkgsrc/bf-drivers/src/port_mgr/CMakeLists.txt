project(libport_mgr VERSION 0.1 LANGUAGES C)

add_library(bfport_mgr_o OBJECT
bf_port_if.c
bf_map.c
port_mgr.c
port_mgr_ha.c
port_mgr_dev.c
port_mgr_port.c
port_mgr_mac_stats.c
port_mgr_map.c
port_mgr_tof1/port_mgr_tof1.c
port_mgr_tof1/port_mgr_tof1_dev.c
port_mgr_tof1/port_mgr_tof1_ha.c
port_mgr_tof1/port_mgr_tof1_port.c
port_mgr_tof1/port_mgr_tof1_map.c
port_mgr_tof1/bf_fsm_if.c
port_mgr_tof1/bf_fsm_hdlrs.c
port_mgr_tof2/port_mgr_tof2.c
port_mgr_tof2/port_mgr_tof2_ha.c
port_mgr_tof2/port_mgr_tof2_map.c
port_mgr_tof2/port_mgr_tof2_dev.c
port_mgr_tof2/port_mgr_tof2_port.c
)
target_compile_options(bfport_mgr_o PRIVATE -Wno-unused-parameter -Wno-maybe-uninitialized -Wno-implicit-fallthrough)
add_library(bfport_mgr SHARED EXCLUDE_FROM_ALL $<TARGET_OBJECTS:bfport_mgr_o>)
target_link_libraries(bfport_mgr lld)
if("${CMAKE_SYSTEM_NAME}" EQUAL "SunOS")
  file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/port_mgr_lib/illumos/libport_mgr_hw.a
    ${CMAKE_CURRENT_SOURCE_DIR}/port_mgr_lib/illumos/libport_mgr_hw.so
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)
else()
  file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/port_mgr_lib/libport_mgr_hw.a
    ${CMAKE_CURRENT_SOURCE_DIR}/port_mgr_lib/libport_mgr_hw.so
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)
endif()

# Building port manager doxygen
find_package(Doxygen)
if(DOXYGEN_FOUND)
  set(srcdir ${CMAKE_CURRENT_SOURCE_DIR}/docs)
  set(doxyfile_in ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
  set(doxyfile ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
  configure_file(${doxyfile_in} ${doxyfile} @ONLY)
  add_custom_target(doc-driver-port-mgr
                    COMMAND ${DOXYGEN_EXECUTABLE} ${doxyfile}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                    COMMENT "Generating Port Manager Doxygen"
                    VERBATIM)
  set_target_properties(doc-driver-port-mgr PROPERTIES EXCLUDE_FROM_ALL TRUE)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/html DESTINATION
    share/doc/bf-drivers/port_mgr OPTIONAL)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/latex DESTINATION
    share/doc/bf-drivers/port_mgr OPTIONAL)
else()
  message(WARNING "Doxygen is needed to build the documentation. Please install doxygen.")
endif()
