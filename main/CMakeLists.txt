idf_component_register(
    SRCS
        "main.cpp"
        "espnow_transport.cpp"
        "provisioning.cpp"
        "chain_node.cpp"
        "gateway_uplink.cpp"
        "video_relay.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_eth driver esp_driver_gpio esp_driver_spi esp_timer lwip
             heap esp_hw_support
)
