idf_component_register(SRCS book.cpp main.cpp max-ascii.cpp tt.cpp eval.cpp eval-stats.cpp san.cpp search.cpp stats.cpp str.cpp test.cpp tui.cpp nnue.cpp nnue_simd.S led_strip_encoder.c
	PRIV_REQUIRES spiffs console esp_driver_uart nvs_flash esp_wifi esp_driver_gpio esp_timer esp_netif esp_http_client esp_driver_usb_serial_jtag esp_driver_rmt esp_netif bootloader_support lwip
	INCLUDE_DIRS . ../include)
spiffs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)

# Scalar fallback image (A/B bench against SIMD, or safe fallback):
#   DOG_NO_SIMD=1 idf.py build
if(DEFINED ENV{DOG_NO_SIMD})
	target_compile_definitions(${COMPONENT_LIB} PRIVATE DOG_NO_SIMD)
endif()
