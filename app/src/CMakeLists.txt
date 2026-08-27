idf_component_register(SRCS book.cpp main.cpp max-ascii.cpp tt.cpp eval.cpp eval-stats.cpp corr_hist.cpp exp_table.cpp san.cpp search.cpp stats.cpp str.cpp test.cpp tui.cpp nnue.cpp web.cpp led_strip_encoder.c
	PRIV_REQUIRES spiffs console esp_driver_uart nvs_flash esp_wifi esp_driver_gpio esp_timer esp_netif esp_http_client esp_http_server esp_driver_usb_serial_jtag esp_driver_rmt esp_netif bootloader_support lwip pthread json esp_adc
	INCLUDE_DIRS . ../include)

target_compile_options(${COMPONENT_LIB} PRIVATE -mtext-section-literals)
spiffs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)
