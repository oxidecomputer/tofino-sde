project(libbf_pm VERSION 0.1 LANGUAGES C)

add_library(bf_pm_o OBJECT
pm_task.c
pm.c
bf_pm_fsm_common.c
bf_pm_intf.c
bf_pm_ucli.c
bf_pm_tof3_ucli.c
port_fsm/bf_pm_fsm_if.c
port_fsm/tof2_fsm/bf_pm_fsm_dfe.c
port_fsm/tof2_fsm/bf_pm_fsm_autoneg.c
port_fsm/tof2_fsm/bf_pm_fsm_prbs.c
port_fsm/tof2_fsm/bf_pm_fsm_mac_loopback.c
port_fsm/tof2_fsm/bf_pm_fsm_pcs_loopback.c
port_fsm/tof2_fsm/bf_pm_fsm_mac_far_loopback.c
port_fsm/tof2_fsm/bf_pm_fsm_serdes_far_loopback.c
port_fsm/tof2_fsm/bf_pm_fsm_pipe_loopback.c
port_fsm/tof2_fsm/bf_pm_fsm_tx_mode.c
port_fsm/tof2_fsm/bf_pm_fsm_sw_model.c
port_fsm/tof2_fsm/bf_pm_fsm_emulator.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_sw_model.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_emulator.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_dfe.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_autoneg.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_prbs.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_pipe_loopback.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_mac_loopback.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_mac_far_loopback.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_pcs_loopback.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_serdes_far_loopback.c
port_fsm/tof3_fsm/bf_pm_fsm_tof3_tx_mode.c
)
set_source_files_properties(bf_pm_ucli.c PROPERTIES COMPILE_FLAGS -Wno-absolute-value)
add_library(bf_pm SHARED EXCLUDE_FROM_ALL $<TARGET_OBJECTS:bf_pm_o>)

# Building platform manager doxygen
find_package(Doxygen)
if(DOXYGEN_FOUND)
  set(srcdir ${CMAKE_CURRENT_SOURCE_DIR}/docs)
  set(doxyfile_in ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in)
  set(doxyfile ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
  configure_file(${doxyfile_in} ${doxyfile} @ONLY)
  add_custom_target(doc-driver-bf-pm
                    COMMAND ${DOXYGEN_EXECUTABLE} ${doxyfile}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                    COMMENT "Generating Platform Manager Doxygen"
                    VERBATIM)
  set_target_properties(doc-driver-bf-pm PROPERTIES EXCLUDE_FROM_ALL TRUE)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/html DESTINATION
    share/doc/bf-drivers/bf_pm OPTIONAL)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/latex DESTINATION
    share/doc/bf-drivers/bf_pm OPTIONAL)
else()
  message(WARNING "Doxygen is needed to build the documentation. Please install doxygen.")
endif()
