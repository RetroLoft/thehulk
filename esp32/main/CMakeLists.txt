idf_component_register(SRCS "main.c" "fpga.c" "hulk.c" "status_led.c" "bootrec.c" "shadow.c" "gemdrv.c" "sd.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_rmt esp_timer nvs_flash esp_hw_support fatfs sdmmc esp_driver_sdspi vfs)

# FPGA bitstream, built by ../fpga (make)
set(FPGA_BITSTREAM "${CMAKE_CURRENT_LIST_DIR}/../../fpga/build/thehulk.bin")
if(NOT EXISTS "${FPGA_BITSTREAM}")
    message(FATAL_ERROR "FPGA bitstream not found: ${FPGA_BITSTREAM}\nRun ./build.sh (or make -C fpga) first.")
endif()
target_add_binary_data(${COMPONENT_TARGET} "${FPGA_BITSTREAM}" BINARY)

# Cartridge image: GEMDRIVE, built by ../atari/gemdrive. Override with -DHULK_ROM_IMAGE=<file>.
set(HULK_ROM_IMAGE "${CMAKE_CURRENT_LIST_DIR}/../../atari/gemdrive/dist/GEMDRIVE.BIN" CACHE FILEPATH "Cartridge image")
if(NOT EXISTS "${HULK_ROM_IMAGE}")
    message(FATAL_ERROR "Cartridge image not found: ${HULK_ROM_IMAGE}\nRun ./build.sh (builds atari/gemdrive) first.")
endif()
configure_file("${HULK_ROM_IMAGE}" "${CMAKE_CURRENT_BINARY_DIR}/rom.img" COPYONLY)
target_add_binary_data(${COMPONENT_TARGET} "${CMAKE_CURRENT_BINARY_DIR}/rom.img" BINARY)
