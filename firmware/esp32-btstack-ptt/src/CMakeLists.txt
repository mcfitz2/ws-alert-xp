# PlatformIO main component.
#   main.c      - IDF entry (app_main) from BTstack's esp32 port; boots BTstack,
#                 calls btstack_main().
#   hfp_ptt.c   - the HFP-AG PTT app; defines btstack_main().
#   a2dp_src.c  - A2DP SBC source (test music) for the coexistence test.
#   sco_demo_util.c - BTstack example SCO audio helper (CVSD/mSBC encoder).
set(BTSTACK "${CMAKE_CURRENT_SOURCE_DIR}/../btstack")

idf_component_register(
    SRCS
        "main.c"
        "hfp_ptt.c"
        "a2dp_src.c"
        "${BTSTACK}/example/sco_demo_util.c"
    INCLUDE_DIRS
        "."
        "${BTSTACK}/example"
    REQUIRES btstack btstack_config esp_driver_gpio bt)

# SCO test audio source: 0 = 266 Hz sine, 2 = MOD player (music, to judge
# CVSD vs mSBC quality by ear).
set_source_files_properties("${BTSTACK}/example/sco_demo_util.c"
    PROPERTIES COMPILE_DEFINITIONS "SCO_DEMO_MODE=2")
