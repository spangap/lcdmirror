include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
# Conditional sources: esp-idf/conditional/<name>/ is compiled only when <name>
# is staged. The LVGL seam (LcdmirrorLcd) lives in conditional/spangap-lcd/ as a
# when:-gated boot-registered service — so the transport core builds headless with
# no #if anywhere, and spangap-lcd is added to REQUIRES only when that slice compiles.
idf_component_register(
    SRCS
        "src/lcdmirror.cpp"
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
)
