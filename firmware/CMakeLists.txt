cmake_minimum_required(VERSION 3.15)

include(proj.conf)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(. main)

target_sources(app PRIVATE
    main/apple_link.c
    main/arrows.c
    main/ble_nav.c
    main/logbuf.c
    main/health.c
    main/backlight.c
    main/nav_state.c
    main/ui.c
)

sdk_set_main_file(main/main.c)

project(bikenav)
