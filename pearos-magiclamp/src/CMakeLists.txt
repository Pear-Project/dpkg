add_subdirectory(kcm)

set(effect_SRCS
    Model.cc
    PearosMagicLampEffect.cc
    plugin.cc
)

kconfig_add_kcfg_files(effect_SRCS
    PearosMagicLampConfig.kcfgc
)

add_library(kwin4_effect_pearosmagiclamp SHARED ${effect_SRCS})

# CMake's default "lib" prefix makes the installed filename
# (libkwin4_effect_pearosmagiclamp.so) disagree with the plugin Id KWin
# derives from it (kwin4_effect_pearosmagiclamp, from metadata.json). KWin's
# effect loader then can't reliably resolve the plugin: it warns about the Id
# mismatch and isEffectSupported()/loadEffect() fail. Match how the other
# kwin effect plugins on this system are built (e.g. magiclamp.so) by
# dropping the prefix.
set_target_properties(kwin4_effect_pearosmagiclamp PROPERTIES PREFIX "")

if(KWIN_PREPAINT_HAS_PRESENT_TIME)
    target_compile_definitions(kwin4_effect_pearosmagiclamp
        PRIVATE KWIN_PREPAINT_HAS_PRESENT_TIME)
endif()

if(KWIN_PAINTWINDOW_RETURNS_BOOL)
    target_compile_definitions(kwin4_effect_pearosmagiclamp
        PRIVATE KWIN_PAINTWINDOW_RETURNS_BOOL)
endif()

target_link_libraries(kwin4_effect_pearosmagiclamp
    Qt6::Core
    Qt6::Gui
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::CoreAddons
    KF6::WindowSystem
    KWin::kwin
)

install(
    TARGETS
        kwin4_effect_pearosmagiclamp

    DESTINATION
        ${KDE_INSTALL_QTPLUGINDIR}/kwin/effects/plugins/
)
