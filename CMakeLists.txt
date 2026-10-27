# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# As far as I know the Raspberry Pi Pico plugin for VSCode does not (yet) support bare RP2350 microcontrollers without a board.
# To adapt the C-SDK for a standard Pico board configure this CMakeLists.txt file via the Raspberry Pi Pico plugin's "Switch Board" option.
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Uncomment the following two lines to compile for bare RP2350 without a board
# set(PICO_PLATFORM rp2350)
# set(PICO_BOARD none CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(hub75_demo C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_subdirectory(utils)

# Add library. This is the core HUB75 driver.
add_library(hub75 STATIC)

target_sources(hub75 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src/hub75.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/rul6024.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/fm6126a.cpp
)

target_link_libraries(hub75 PUBLIC
        pico_stdlib
        pico_multicore
        pico_aon_timer
        hardware_pio
        hardware_dma
        hardware_vreg
        pico_graphics
)

target_include_directories(hub75 PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_include_directories(hub75 PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
)

pico_generate_pio_header(hub75 ${CMAKE_CURRENT_LIST_DIR}/src/hub75.pio)

# target_compile_definitions are PUBLIC so any target that links "hub75" (e.g. hub75_demo or hub75_demo_dual below, or a
# consumer project's own executable) automatically inherits the exact same panel configuration.
target_compile_definitions(hub75 PUBLIC
    # PICO_RP2350A=0         # PICO_RP2350A=0` means not a RP2350A but a RP2350B microcontroller - uncomment for RP235xB microcontroller only!
    USE_PICO_GRAPHICS=false  # diagnostic uses a compact raw BGR framebuffer
    HUB75_MULTICORE=true     # use core1 for the hub75 driver
)

# Add demo executable. Everything that is only required for the demo remains here.
add_executable(hub75_demo
        hub75_demo.cpp
        aircraft_data.c
        flightwall_settings.c
        update_client.c
        wifi_provisioning.c
        ota_server.c
        dhcpserver/dhcpserver.c
        dnsserver/dnsserver.c
)

pico_set_program_name(hub75_demo "hub75_demo")
pico_set_program_version(hub75_demo "1.6.1")

pico_set_float_implementation(hub75_demo pico)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(hub75_demo 0)
pico_enable_stdio_usb(hub75_demo 1)

add_subdirectory(common)
add_subdirectory(libraries)

target_compile_definitions(hub75_demo PRIVATE
        PICO_CRT0_IMAGE_TYPE_TBYB=1
        PICO_FLASH_BANK_STORAGE_OFFSET=0x3EB000
        CYW43_HOST_NAME="flightwall"
        FLIGHTWALL_VERSION="1.6.1"
        FLIGHTWALL_BUILD_NUMBER=10601
)

target_link_libraries(hub75_demo PRIVATE
        hub75
        pico_cyw43_arch_lwip_threadsafe_background
        pico_lwip_http
        pico_lwip_mbedtls
        pico_mbedtls
        pico_sha256
        boot_uf2_headers
        hardware_flash
)

# Add the example files to the build
target_include_directories(hub75_demo PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/examples
        ${CMAKE_CURRENT_LIST_DIR}
        ${PICO_LWIP_CONTRIB_PATH}/apps/httpd
        ${CMAKE_CURRENT_LIST_DIR}/dhcpserver
        ${CMAKE_CURRENT_LIST_DIR}/dnsserver
)

pico_add_library(flightwall_web_content NOFLAG)
pico_set_lwip_httpd_content(flightwall_web_content INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/content/index.shtml
        ${CMAKE_CURRENT_LIST_DIR}/content/404.html
        ${CMAKE_CURRENT_LIST_DIR}/content/404.response
        ${CMAKE_CURRENT_LIST_DIR}/content/update-ok.html
        ${CMAKE_CURRENT_LIST_DIR}/content/update-failed.html
        ${CMAKE_CURRENT_LIST_DIR}/content/settings-saved.html
        ${CMAKE_CURRENT_LIST_DIR}/content/settings-failed.html
)
target_link_libraries(hub75_demo PRIVATE flightwall_web_content)

pico_hash_binary(hub75_demo)

# Release builds should be signed with a project-specific private key that is
# kept outside the repository. Pass its path with either the CMake cache or an
# environment variable:
#   cmake -S . -B build -DFLIGHTWALL_SIGNING_KEY=/secure/path/private.pem
set(FLIGHTWALL_SIGNING_KEY "" CACHE FILEPATH "Private key used to sign FlightWall firmware")
if(NOT FLIGHTWALL_SIGNING_KEY AND DEFINED ENV{FLIGHTWALL_SIGNING_KEY})
    set(FLIGHTWALL_SIGNING_KEY "$ENV{FLIGHTWALL_SIGNING_KEY}")
endif()

if(FLIGHTWALL_SIGNING_KEY)
    if(NOT EXISTS "${FLIGHTWALL_SIGNING_KEY}")
        message(FATAL_ERROR "FLIGHTWALL_SIGNING_KEY does not exist: ${FLIGHTWALL_SIGNING_KEY}")
    endif()
    pico_sign_binary(hub75_demo "${FLIGHTWALL_SIGNING_KEY}")
else()
    message(WARNING "Building unsigned FlightWall firmware. Set FLIGHTWALL_SIGNING_KEY for a signed release build.")
endif()

pico_add_extra_outputs(hub75_demo)
print_size_info(hub75_demo)


# Add dual-panel demo executable - two independent Hub75Driver instances, see hub75_demo_dual.cpp for details.
add_executable(hub75_demo_dual hub75_demo_dual.cpp)

pico_set_program_name(hub75_demo_dual "hub75_demo_dual")
pico_set_program_version(hub75_demo_dual "3.1")

pico_set_float_implementation(hub75_demo_dual pico)

target_compile_definitions(hub75_demo_dual PRIVATE
    USE_PICO_GRAPHICS=true
)

pico_enable_stdio_uart(hub75_demo_dual 0)
pico_enable_stdio_usb(hub75_demo_dual 1)

target_link_libraries(hub75_demo_dual PRIVATE
        hub75)

target_include_directories(hub75_demo_dual PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/examples
)

pico_add_extra_outputs(hub75_demo_dual)
print_size_info(hub75_demo_dual)
