cmake_minimum_required(VERSION 3.16)
project(ts3_soundboard VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Plugin-Version (wird vom publish.ps1 via -DPLUGIN_VERSION=x.y.z übergeben) ───
if(NOT DEFINED PLUGIN_VERSION)
    set(PLUGIN_VERSION "1.0.0")
endif()

# ── TS3 SDK Pfad ───
# Das TS3 Client Plugin SDK hier eintragen
set(TS3_SDK_DIR "${CMAKE_SOURCE_DIR}/ts3client-pluginsdk-26" CACHE PATH "Pfad zum TS3 Plugin SDK")

# ── Shared Library (DLL) ──────────────────────────────────────────────────────
add_library(ts3_soundboard SHARED
    src/plugin.cpp
    src/audio_mixer.cpp
    src/pipe_server.cpp
    src/config.cpp
    src/settings_dialog.cpp
)

target_include_directories(ts3_soundboard PRIVATE
    src/
    ${TS3_SDK_DIR}/include
)

# PLUGIN_VERSION als Compiler-Define einsetzen (plattformunabhängig)
target_compile_definitions(ts3_soundboard PRIVATE PLUGIN_VERSION="${PLUGIN_VERSION}")

# ── Windows-spezifische Einstellungen ───
if(WIN32)
    target_compile_definitions(ts3_soundboard PRIVATE WIN32_LEAN_AND_MEAN)
    target_link_libraries(ts3_soundboard PRIVATE comctl32 gdiplus)

    # Ausgabe-DLL direkt in TS3 Plugin-Ordner kopieren (optional)
    # set(TS3_PLUGIN_DIR "$ENV{APPDATA}/TS3Client/plugins")
    # add_custom_command(TARGET ts3_soundboard POST_BUILD
    #     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ts3_soundboard> "${TS3_PLUGIN_DIR}/ts3_soundboard.dll"
    # )
endif()

# Output-Name (TS3 erwartet eine .dll ohne Prefix)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH_SUFFIX "_win64")
else()
    set(ARCH_SUFFIX "_win32")
endif()

set_target_properties(ts3_soundboard PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ts3_soundboard${ARCH_SUFFIX}"
)

# Locale-Dateien in den ts3_soundboard-Unterordner neben die DLL kopieren
add_custom_command(TARGET ts3_soundboard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/locales"
        "$<TARGET_FILE_DIR:ts3_soundboard>/ts3_soundboard/locales"
    COMMENT "Kopiere locales/ nach ts3_soundboard/locales/"
)
