# SPDX-License-Identifier: MIT
set(ROM_INPUT "" CACHE FILEPATH "Optional externally supplied Apple ROM: 8192, 12288 or 16384 bytes")
set(ROM_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rom_image.c")
set(DISK_ROM_INPUT "" CACHE FILEPATH "Optional externally supplied 256-byte Disk II slot ROM")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/embed_rom.py"
    --output "${ROM_OUTPUT}" --input "${ROM_INPUT}" --disk-rom "${DISK_ROM_INPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
if(ROM_INPUT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ROM_INPUT}")
endif()
if(DISK_ROM_INPUT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DISK_ROM_INPUT}")
endif()
set(FPGA_UART_BAUD "115200" CACHE STRING "Baud rate matching the FPGA UART_BAUD build parameter")
if(NOT FPGA_UART_BAUD MATCHES "^[0-9]+$")
    message(FATAL_ERROR "FPGA_UART_BAUD must be an integer")
endif()
set(PRIVATE_CONFIG "" CACHE FILEPATH "Ignored local Wi-Fi/API-token JSON")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/embed_config.py"
    --output "${CMAKE_CURRENT_BINARY_DIR}/private_config.h" --input "${PRIVATE_CONFIG}"
    COMMAND_ERROR_IS_FATAL ANY)
if(PRIVATE_CONFIG)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PRIVATE_CONFIG}")
endif()
idf_component_register(SRCS "main.c" "apple2.c" "disk2.c" "shell_link.c" "network.c" "wifi_settings.c" "wifi_store.c" "wifi_portal.c"
    "video_render.c" "video_delta.c" "video_link.c" "cart_commands.c" "cart_storage.c" "handheld_ui.c" "audio.c"
    "../../storage/a2_storage.c" "../../storage/a2_is29gl032.c" "../../storage/a2_import.c"
    "../../storage/a2_disk_overlay.c"
    "../../storage/a2_dos_catalog.c"
    "ssh/ssh_transport.cpp" "ssh/ssh_platform_esp32.c" "ssh/ssh_terminal.c"
    "ssh/vendor/staticnet/crypt/CryptoEngine.cpp"
    "ssh/vendor/staticnet/contrib/tweetnacl_25519.cpp"
    "ssh/vendor/staticnet/contrib/base64.cpp"
    "../../storage/vendor/littlefs/lfs.c" "../../storage/vendor/littlefs/lfs_util.c"
    "vendor/mii/mii_65c02.c" "${ROM_OUTPUT}"
    INCLUDE_DIRS "." "vendor/mii" "../../storage" "ssh" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp_driver_uart esp_driver_spi esp_driver_i2s console esp_timer esp_wifi esp_netif esp_event esp_http_server nvs_flash mbedtls lwip)
target_compile_definitions(${COMPONENT_LIB} PRIVATE MII_RP2350=0 MII_65C02_DIRECT_ACCESS=1
    FPGA_UART_BAUD=${FPGA_UART_BAUD} LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)
