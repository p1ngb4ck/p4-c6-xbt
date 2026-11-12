set(srcs "c6_flash.c")
if(CONFIG_C6_XBT)
    list(APPEND srcs "c6_xbt.c")
endif()

set(priv esp_hosted bt esp_hid esp_ringbuf esp_timer heap nvs_flash app_update esp_partition esp_system
    bootloader_support spi_flash esp_rom espcoredump)
foreach(optional esp_mspi esp_hal_wdt)
    if(EXISTS "$ENV{IDF_PATH}/components/${optional}")
        list(APPEND priv ${optional})
    endif()
endforeach()

set(embed "")
if(NOT CONFIG_C6_XBT)
    set(embed "table_xbt.bin" "table_stock.bin")
endif()

idf_component_register(
    SRCS          ${srcs}
    INCLUDE_DIRS  "."
    PRIV_REQUIRES ${priv}
    EMBED_FILES   ${embed}
)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u c6_flash_boot")
if(CONFIG_C6_XBT)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u c6_xbt_init")
endif()
