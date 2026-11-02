idf_component_register(SRCS "main.c" "fpga.c" "hulk.c" "status_led.c" "bootrec.c" "shadow.c" "gemdrv.c" "gemdrv_fs.c" "sd.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_rmt esp_timer nvs_flash esp_hw_support fatfs sdmmc esp_driver_sdspi vfs)

# FPGA bitstream, built by ../fpga (make)
set(FPGA_BITSTREAM "${CMAKE_CURRENT_LIST_DIR}/../../fpga/build/thehulk.bin")
if(NOT EXISTS "${FPGA_BITSTREAM}")
    message(FATAL_ERROR "FPGA bitstream not found: ${FPGA_BITSTREAM}\nRun ./build.sh (or make -C fpga) first.")
endif()
target_add_binary_data(${COMPONENT_TARGET} "${FPGA_BITSTREAM}" BINARY)

# The configuration program on the flash drive, built by ../atari/config (make)
set(HULK_CONFIG_PRG "${CMAKE_CURRENT_LIST_DIR}/../../atari/config/THEHULK.PRG")
if(NOT EXISTS "${HULK_CONFIG_PRG}")
    message(FATAL_ERROR "Configuration program not found: ${HULK_CONFIG_PRG}\nRun make in atari/config first.")
endif()
target_add_binary_data(${COMPONENT_TARGET} "${HULK_CONFIG_PRG}" BINARY)

# Cartridge image: GEMDRIVE, built by ../atari/gemdrive. Override with -DHULK_ROM_IMAGE=<file>.
set(HULK_ROM_IMAGE "${CMAKE_CURRENT_LIST_DIR}/../../atari/gemdrive/dist/GEMDRIVE.BIN" CACHE FILEPATH "Cartridge image")
if(NOT EXISTS "${HULK_ROM_IMAGE}")
    message(FATAL_ERROR "Cartridge image not found: ${HULK_ROM_IMAGE}\nRun ./build.sh (builds atari/gemdrive) first.")
endif()
# Diagnostics: configure the FPGA but never enable the cartridge. The board stays in the
# Atari, which then starts as without a cartridge.
option(HULK_CART_OFF "Never enable the cartridge" OFF)
if(HULK_CART_OFF)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE HULK_CART_OFF)
endif()

# Diagnostics: behave as a plain read-only cartridge, whatever HULK_ROM_IMAGE holds:
# no GEMDRIVE shared memory and no answers to commands.
option(HULK_CART_READONLY "Plain read-only cartridge" OFF)
if(HULK_CART_READONLY)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE HULK_CART_READONLY)
endif()

# Diagnostics: GEMDRIVE starts at boot like always, but returns at once: nothing on the
# screen, no commands, no GEMDOS hook and no drive.
option(HULK_GEMDRIVE_OFF "GEMDRIVE does not install itself" OFF)
if(HULK_GEMDRIVE_OFF)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE HULK_GEMDRIVE_OFF)
endif()

# Diagnostics: upload the whole image before the cartridge is enabled
option(HULK_UPLOAD_ALL "Upload the whole image before enabling" OFF)
if(HULK_UPLOAD_ALL)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE HULK_UPLOAD_ALL)
endif()

# Diagnostics: 1 = GEMDRIVE boots and sends its commands, but installs no drive and no
# GEMDOS hook; 2 = it installs the hook, but no drive; 3 = it installs everything. All three
# log the commands and file calls.
set(HULK_GEMDRIVE_NO_INSTALL 0 CACHE STRING "GEMDRIVE install level")
target_compile_definitions(${COMPONENT_TARGET} PRIVATE HULK_GEMDRIVE_NO_INSTALL=${HULK_GEMDRIVE_NO_INSTALL})

# Diagnostics: GEMDRIVE sends the TOS 4 ROM to the ESP32 log
option(HULK_TOS_DUMP "Dump the TOS 4 ROM to the log" OFF)
if(HULK_TOS_DUMP)
    target_compile_definitions(${COMPONENT_TARGET} PRIVATE TOS_DUMP=1)
endif()

configure_file("${HULK_ROM_IMAGE}" "${CMAKE_CURRENT_BINARY_DIR}/rom.img" COPYONLY)
target_add_binary_data(${COMPONENT_TARGET} "${CMAKE_CURRENT_BINARY_DIR}/rom.img" BINARY)
