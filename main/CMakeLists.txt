idf_component_register(SRCS "main.c" "display.c" "visualizer.c" "power_button.c" "serial_control.c" "codec/es8311.c"
                       INCLUDE_DIRS "." "codec"
                       REQUIRES driver esp_driver_i2c esp_driver_i2s esp_driver_gpio
                                esp_driver_spi esp_lcd usb_device_uac
                                espressif__tinyusb espressif__esp_lcd_co5300
                                espressif__esp_lcd_panel_io_additions)

# UAC 1.3.1 exposes custom descriptors via USB_DEVICE_UAC_AS_PART but keeps
# its TinyUSB configuration headers private. Share those pinned headers with
# both the stack and our single project-owned string descriptor callback.
set(uac_dir "${CMAKE_SOURCE_DIR}/managed_components/espressif__usb_device_uac")
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(uac_lib espressif__usb_device_uac COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC "${uac_dir}/tusb" "${uac_dir}/tusb_uac")
target_include_directories(${uac_lib} PUBLIC "${uac_dir}/tusb" "${uac_dir}/tusb_uac")
set(cdc_definitions CFG_TUD_CDC=1 CFG_TUD_CDC_RX_BUFSIZE=256
                    CFG_TUD_CDC_TX_BUFSIZE=256 CFG_TUD_CDC_EP_BUFSIZE=64)
target_compile_definitions(${tusb_lib} PUBLIC ${cdc_definitions})
target_compile_definitions(${uac_lib} PUBLIC ${cdc_definitions})
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${cdc_definitions})
target_sources(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/usb/usb_descriptors.c")
