idf_component_register(
    SRCS "main.c" "app.c" "sco_audio.c"
    INCLUDE_DIRS "."
    EMBED_FILES "commentary.ul"
    REQUIRES ws6_ptt btstack btstack_config esp_driver_gpio bt)
