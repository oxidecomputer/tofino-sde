cmake_minimum_required(VERSION 3.5)
project(bf_switchd)

add_library(driver_o OBJECT
  bf_switchd_i2c.c
  bf_switchd.c
  bf_switchd_log.c
  switch_config.c
  switchd_ucli.c
  bf_hw_porting_config.c
  switch_config.h
  bf_model_pltfm_porting.c
  diag_py.c
)
add_library(driver SHARED
  $<TARGET_OBJECTS:driver_o>
  $<TARGET_OBJECTS:pdfixed_o>
  $<TARGET_OBJECTS:bftraffic_mgr_o>
  $<TARGET_OBJECTS:ctx_json_o>
  $<TARGET_OBJECTS:dvm_o>
  $<TARGET_OBJECTS:diag_o>
  $<TARGET_OBJECTS:lld_o>
  $<TARGET_OBJECTS:bfpipe_mgr_o>
  $<TARGET_OBJECTS:bfpkt_mgr_o>
  $<TARGET_OBJECTS:bfmc_mgr_o>
  $<TARGET_OBJECTS:bfport_mgr_o>
  $<TARGET_OBJECTS:bf_pm_o>
  $<TARGET_OBJECTS:knet_mgr_o>
  $<TARGET_OBJECTS:perf_o>
  $<$<BOOL:${BFRT}>:$<TARGET_OBJECTS:bfrt_o>>
  $<$<BOOL:${TDI}>:$<TARGET_OBJECTS:tdi_tofino_o>>
  $<$<BOOL:${TDI}>:$<TARGET_OBJECTS:tdi_o>>
  $<$<BOOL:${TDI}>:$<TARGET_OBJECTS:tdi_tna_o>>
  $<$<BOOL:${TDI}>:$<TARGET_OBJECTS:tdi_json_parser_o>>
  $<$<BOOL:${PI}>:$<TARGET_OBJECTS:bfpi_o>>
  $<$<BOOL:${P4RT}>:$<TARGET_OBJECTS:bfp4rt_o>>
  $<$<BOOL:${GRPC}>:$<TARGET_OBJECTS:bf_google_grpc_o>>
  $<$<BOOL:${GRPC}>:$<TARGET_OBJECTS:bf_google_protobuf_o>>
  $<$<BOOL:${THRIFT-DRIVER}>:$<TARGET_OBJECTS:pdfixed_thrift_o>>
)
set_target_properties(driver PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(driver PUBLIC bfutils dl pthread m target_sys target_utils)

find_program(MY_PROGRAM libport_mgr_hw.a
           HINTS ${CMAKE_INSTALL_PREFIX}/lib)
if(MY_PROGRAM)
  message(STATUS "Found my_program: ${MY_PROGRAM}")
else()
  message(FATAL_ERROR "my_program not found ${CMAKE_INSTALL_PREFIX}")
endif()

target_link_libraries(driver PRIVATE ${MY_PROGRAM})

if (NOT STANDALONE)
  target_link_libraries(driver PUBLIC clish)
endif()

if(GRPC)
  find_package(Protobuf REQUIRED)
  find_package(GRPC REQUIRED)
  target_link_libraries(driver PUBLIC ${PROTOBUF_LIBRARY} ${GRPC_LIBRARY} ${GRPC_GRPC++_LIBRARY} ${GRPC_GRPC++_REFLECTION_LIBRARY})
endif()
if (PI)
  find_library(PI_ALL_LIB NAMES piall HINTS ${CMAKE_INSTALL_PREFIX}/lib REQUIRED)
  if (GRPC)
    find_library(PI_PROTOGRPC_LIB NAMES piprotogrpc HINTS ${CMAKE_INSTALL_PREFIX}/lib REQUIRED)
    find_library(PI_PROTOBUF_LIB NAMES piprotobuf HINTS ${CMAKE_INSTALL_PREFIX}/lib REQUIRED)
    target_link_libraries(driver PUBLIC ${PI_ALL_LIB} ${PI_PROTOGRPC_LIB} ${PI_PROTOBUF_LIB})
  else()
    target_link_libraries(driver PUBLIC ${PI_ALL_LIB})
  endif()
endif()
if (P4RT)
  find_library(PI_FEPROTO_LIB NAMES pifeproto HINTS ${CMAKE_INSTALL_PREFIX}/lib REQUIRED)
  target_link_libraries(driver PUBLIC ${PI_FEPROTO_LIB})
endif()
if (THRIFT-DRIVER)
  target_link_libraries(driver PUBLIC ${THRIFT_LIBRARIES})
endif()

if (NOT STATIC-LINK-LIB)
  add_executable(bf_switchd bf_switchd_main.c)
  target_link_libraries(bf_switchd driver)
  install(TARGETS bf_switchd DESTINATION bin)
endif()

file(COPY bf_switchd.h DESTINATION ${CMAKE_INSTALL_PREFIX}/include/bf_switchd)
install(FILES zlog-cfg DESTINATION ${CMAKE_INSTALL_PREFIX}/share/bf_switchd)
