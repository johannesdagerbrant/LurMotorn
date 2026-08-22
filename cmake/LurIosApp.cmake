# lur_add_ios_app — the whole iOS app shell for a game, in one call (#198).
#
# The two games' iOS CMakeLists were the same file twice: the MoltenVK slice pick, the .mm source
# list, the ARC flags, the eleven frameworks, the bundle properties and the Info.plist configure. RPS's
# even said so — "Copy-first: the platform seams are duplicated from chess". What actually differed was
# the app name, the bundle id, the BLE UUID, the game's own libraries and one sentence of Bluetooth
# usage prose.
#
# That mattered beyond tidiness: the same duplication is where LUR_AGENT went missing from RPS's copy
# (see LurApp.cmake). Templates do not drift.
#
# Requires the engine root to have been added already (this function uses lur_configure_app_target and
# links lur::* targets), and MOLTENVK_DIR to be set — iOS has no system Vulkan.
#
#   lur_add_ios_app(OnlyRps
#       BUNDLE_ID   com.lurmotorn.onlyrps
#       LOG_TAG     OnlyRps
#       BLE_UUID    4C55524D-...
#       USAGE_NOUN  "local two-player RTS"
#       MAIN        Sources/RpsMain.mm
#       GAME_LIBS   rps::app rps::view rps::core)

include_guard(GLOBAL)

function(lur_add_ios_app APP)
    cmake_parse_arguments(LIOS "" "BUNDLE_ID;LOG_TAG;BLE_UUID;USAGE_NOUN;MAIN" "GAME_LIBS" ${ARGN})
    foreach(req BUNDLE_ID LOG_TAG BLE_UUID USAGE_NOUN)
        if(NOT LIOS_${req})
            message(FATAL_ERROR "lur_add_ios_app(${APP}): ${req} is required")
        endif()
    endforeach()
    if(NOT LIOS_GAME_LIBS)
        message(FATAL_ERROR "lur_add_ios_app(${APP}): GAME_LIBS is required (the game's own targets)")
    endif()
    # The app's own ObjC++ entry point. A parameter rather than a fixed convention because the two
    # games already disagree (Sources/AppMain.mm vs Sources/RpsMain.mm), and renaming a file to
    # satisfy a template is the template serving itself.
    if(NOT LIOS_MAIN)
        set(LIOS_MAIN "Sources/AppMain.mm")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${LIOS_MAIN}")
        message(FATAL_ERROR "lur_add_ios_app(${APP}): MAIN '${LIOS_MAIN}' does not exist")
    endif()
    if(NOT DEFINED MOLTENVK_DIR)
        message(FATAL_ERROR
            "MOLTENVK_DIR is not set — point it at the extracted MoltenVK package. iOS has no system "
            "Vulkan; MoltenVK is the one sanctioned runtime third-party dependency.")
    endif()

    get_filename_component(LUR_ROOT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)

    # The static xcframework slice has to match the SDK, or the link fails with a mess of missing
    # symbols that reads like a MoltenVK packaging problem rather than the wrong architecture.
    if(CMAKE_OSX_SYSROOT MATCHES "simulator")
        set(MVK_SLICE "ios-arm64_x86_64-simulator")
    else()
        set(MVK_SLICE "ios-arm64")
    endif()
    set(MVK_INCLUDE "${MOLTENVK_DIR}/include")
    set(MVK_LIB "${MOLTENVK_DIR}/static/MoltenVK.xcframework/${MVK_SLICE}/libMoltenVK.a")

    # The shared Vulkan backend is compiled for iOS here, so it needs the MoltenVK headers.
    target_include_directories(lur_render PRIVATE ${MVK_INCLUDE})

    # The engine's iOS platform seams. They live in the APP target rather than in their modules
    # because they are ObjC++ and need the app's LUR_LOG_TAG — and because only an app build has an
    # iOS toolchain at all. One copy per platform, shared by every game.
    set(_shell_sources
        ${LUR_ROOT}/Modules/Transport/Platform/Ios/BleTransport.mm
        ${LUR_ROOT}/Modules/App/Platform/Ios/AppPlatform.mm
        ${LUR_ROOT}/Modules/App/Platform/Ios/IosViewHost.mm
        ${LUR_ROOT}/Modules/App/Platform/Ios/IosApp.mm)

    add_executable(${APP} MACOSX_BUNDLE ${LIOS_MAIN} ${_shell_sources})
    target_include_directories(${APP} PRIVATE ${MVK_INCLUDE})

    # ARC for every ObjC++ translation unit, the app's own included.
    set_source_files_properties(${LIOS_MAIN} ${_shell_sources}
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")

    lur_configure_app_target(${APP} LOG_TAG ${LIOS_LOG_TAG} BLE_UUID ${LIOS_BLE_UUID})

    target_link_libraries(${APP} PRIVATE
        ${LIOS_GAME_LIBS}
        lur::render
        lur::transport
        lur::net
        lur::app     # GameHost — the session + persistence choreography, engine-owned
        lur::save    # persistent device id — a stable BLE role across restarts
        lur::sim
        lur::math
        lur::audio   # wait-free SFX mixer + LSF1 codec, incl. its RemoteIO device seam
        lur::trace   # named CPU scopes + the touch->present latency
        ${MVK_LIB}
        "-framework Foundation"
        "-framework UIKit"
        "-framework CoreBluetooth"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework IOSurface"
        "-framework CoreGraphics"
        "-framework AudioToolbox"   # RemoteIO Audio Unit (lur::audio's iOS device seam)
        "-framework AVFAudio")      # AVAudioSession (playback category + IO buffer)

    set_target_properties(${APP} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${LIOS_BUNDLE_ID}"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "13.0"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1")

    # One shared plist template. The Bluetooth usage strings are what the OS shows the user in the
    # permission prompt, so they must name the actual game — hence USAGE_NOUN rather than a generic
    # sentence. "No internet, no servers" is in both because it is true of the engine, not of a game.
    set(LUR_APP_NAME "${APP}")
    set(LUR_APP_BUNDLE_ID "${LIOS_BUNDLE_ID}")
    set(LUR_APP_USAGE_NOUN "${LIOS_USAGE_NOUN}")
    configure_file("${LUR_ROOT}/Games/Shared/iOS/Info.plist.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
endfunction()
