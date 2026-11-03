idf_component_register(
    SRCS "pet_pack.c" "pet_anim.c" "pet_imu.c" "pet_pull.c"
    INCLUDE_DIRS "include" "."
    REQUIRES esp_jpeg esp_new_jpeg esp_driver_i2c esp_timer esp_http_client mbedtls esp_partition
)
