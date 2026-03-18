cmake_minimum_required(VERSION 3.10)
project(mqtt_dashboard C)

set(CMAKE_C_STANDARD 11)

find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_library(WEBSOCKETS_LIB websockets REQUIRED)
find_library(CURL_LIB curl)

add_executable(mqtt_dashboard
    src/main.c
    src/cJSON.c
    src/auth.c
    src/seat_manager.c
    src/pricing.c
    src/qrcodegen.c
    src/qr_svg.c
)

target_include_directories(mqtt_dashboard PRIVATE src)

if(CURL_LIB)
    message(STATUS "libcurl found — InfluxDB + distance pricing support enabled")
    target_sources(mqtt_dashboard PRIVATE src/influxdb.c src/routes.c)
    target_link_libraries(mqtt_dashboard ${MOSQUITTO_LIB} ${WEBSOCKETS_LIB} ${CURL_LIB})
    target_compile_definitions(mqtt_dashboard PRIVATE HAS_INFLUXDB=1)
else()
    message(STATUS "libcurl NOT found — InfluxDB + distance pricing disabled (install libcurl4-openssl-dev)")
    target_sources(mqtt_dashboard PRIVATE src/influxdb_stub.c src/routes_stub.c)
    target_link_libraries(mqtt_dashboard ${MOSQUITTO_LIB} ${WEBSOCKETS_LIB})
endif()

# Tell the binary where to find the www/ directory
target_compile_definitions(mqtt_dashboard PRIVATE
    WWW_DIR="${CMAKE_SOURCE_DIR}/www"
)

# Security: override PINs at build time so they never appear in source
# Usage: cmake -B build -DCONDUCTOR_PIN=9876 -DOWNER_PIN=5432
if(DEFINED CONDUCTOR_PIN)
    target_compile_definitions(mqtt_dashboard PRIVATE
        CONDUCTOR_PIN="${CONDUCTOR_PIN}")
    message(STATUS "CONDUCTOR_PIN overridden at build time")
endif()
if(DEFINED OWNER_PIN)
    target_compile_definitions(mqtt_dashboard PRIVATE
        OWNER_PIN="${OWNER_PIN}")
    message(STATUS "OWNER_PIN overridden at build time")
endif()
