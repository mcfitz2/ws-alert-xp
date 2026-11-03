# PlatformIO main component.
#   main.c      - IDF entry (app_main) from BTstack's esp32 port; boots BTstack,
#                 calls btstack_main().
#   hfp_ptt.c   - the HFP-AG PTT app; defines btstack_main().
#   a2dp_src.c  - A2DP SBC source (test music) for the coexistence test.
#   sco_audio.c - streams commentary.ul (FM stand-in) over SCO, muted on PTT.
#   sco_demo_util.c - BTstack example SCO audio helper (sets voice setting).
set(BTSTACK "${CMAKE_CURRENT_SOURCE_DIR}/../btstack")

idf_component_register(
    SRCS
        "main.c"
        "hfp_ptt.c"
        "a2dp_src.c"
        "sco_audio.c"
        "${BTSTACK}/example/sco_demo_util.c"
    INCLUDE_DIRS
        "."
        "${BTSTACK}/example"
    EMBED_FILES "commentary.ul"
    REQUIRES btstack btstack_config esp_driver_gpio bt)

