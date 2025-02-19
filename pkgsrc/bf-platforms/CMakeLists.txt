cmake_minimum_required(VERSION 3.5)

option(SIDECAR "Build Sidecar Platform" ON)

set(CMAKE_SHARED_LINKER_FLAGS "-Wl,-rpath,'\$ORIGIN'")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}     -DINC_PLTFM_UCLI -Wno-unused-parameter -Wno-shadow")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DINC_PLTFM_UCLI -Wno-unused-parameter -Wno-shadow")

if (ASIC)
  add_definitions(-DDEVICE_IS_ASIC)
else()
  add_definitions(-DDEVICE_IS_SW_MODEL)
endif()

include_directories(platforms/include)
include_directories(drivers/src)
include_directories(drivers/include)
include_directories(platforms/common/devices/include)

add_library(pltfm_o OBJECT
  drivers/src/bf_pltfm_mgr/pltfm_mgr_init.c
  drivers/src/bf_pltfm_mgr/pltfm_mgr_handlers.c
)

set(SIDECAR_PLTFM_SRCS
  platforms/sidecar/src/platform_mgr/platform.c
  platforms/sidecar/src/platform_mgr/board.c
)
add_library(sidecar_driver_o OBJECT
  ${SIDECAR_PLTFM_SRCS}
)
add_library(sidecar_driver SHARED $<TARGET_OBJECTS:sidecar_driver_o>)

# Promise the platform-specific symbols via a mapfile.
#
# This allows us to write the symbols implementing most of our platform
# support code in Rust. We promise the compiler driver that those symbols will
# be available to the runtime linker, so that linking of this library can
# succeed. The symbols defined in the mapfile are actually implemented in the
# Dendrite repository.
if("${CMAKE_SYSTEM_NAME}" EQUAL "SunOS")
  set(
      SIDECAR_MAPFILE
      "${CMAKE_CURRENT_SOURCE_DIR}/platforms/sidecar/src/platform_mgr/mapfile"
  )
  set_property(
      TARGET
      sidecar_driver
      APPEND
      PROPERTY
      LINK_DEPENDS
      ${SIDECAR_MAPFILE}
  )
  target_link_options(sidecar_driver PUBLIC "-Wl,-M,${SIDECAR_MAPFILE}")
endif()

add_library(pltfm_driver_o OBJECT
  drivers/src/bf_bd_cfg/bf_bd_cfg_intf.c
  drivers/src/bf_bd_cfg/bf_bd_cfg_intf_ucli.c
  drivers/src/bf_bd_cfg/bf_bd_cfg_porting.c
  drivers/src/bf_qsfp/bf_qsfp_cli.c
  drivers/src/bf_qsfp/bf_qsfp_comm.c
  drivers/src/bf_port_mgmt/bf_pm_intf.c
  drivers/src/bf_port_mgmt/bf_pm_intf_new.c
  drivers/src/bf_port_mgmt/bf_pm_qsfp_mgmt.c
  drivers/src/bf_port_mgmt/bf_pm_priv.h
  drivers/src/bf_port_mgmt/bf_pm_porting.c
)
add_library(pltfm_driver SHARED $<TARGET_OBJECTS:pltfm_driver_o>)
target_link_libraries(pltfm_driver target_sys curl)

add_library(pltfm_mgr SHARED
  $<TARGET_OBJECTS:pltfm_o>
  $<TARGET_OBJECTS:pltfm_driver_o>
  $<TARGET_OBJECTS:sidecar_driver_o>
)

target_link_libraries(pltfm_mgr PUBLIC target_sys curl)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/platforms/sidecar/src/board-maps/
  DESTINATION share/platforms/board-maps/oxide
  FILES_MATCHING
  PATTERN "sidecar*.csv"
)
# platform generic headers
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/platforms/include/bf_pltfm_qsfp.h DESTINATION include/bf_pltfm)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/platforms/include/bf_pltfm_led.h DESTINATION include/bf_pltfm)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/platforms/include/bf_pltfm_bd_cfg.h DESTINATION include/bf_pltfm)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/platforms/include/bf_pltfm_ext_phy.h DESTINATION include/bf_pltfm)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/platforms/include/bf_pltfm.h DESTINATION include/bf_pltfm)

# platform driver headers
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_bd_cfg/bf_bd_cfg_intf.h DESTINATION include/bf_bd_cfg)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_bd_cfg/bf_bd_cfg_porting.h DESTINATION include/bf_bd_cfg)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_bd_cfg/bf_bd_cfg_bd_map.h DESTINATION include/bf_bd_cfg)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_pltfm_types/bf_pltfm_types.h DESTINATION include/bf_pltfm_types)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_port_mgmt/bf_port_mgmt_intf.h DESTINATION include/bf_port_mgmt)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_port_mgmt/bf_port_mgmt_porting.h DESTINATION include/bf_port_mgmt)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_pltfm_mgr/pltfm_mgr_handlers.h DESTINATION include/bf_pltfm_mgr)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_led/bf_led.h DESTINATION include/bf_led)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_qsfp/bf_qsfp.h DESTINATION include/bf_qsfp)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include/bf_qsfp/sff.h DESTINATION include/bf_qsfp)
