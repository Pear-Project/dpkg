set(effect_SRCS
    PearosTinterEffect.cc
    WallpaperSampler.cc
    plugin.cc
    shaders.qrc
)

kconfig_add_kcfg_files(effect_SRCS
    PearosTinterConfig.kcfgc
)

add_library(kwin4_effect_pearostinter SHARED ${effect_SRCS})

# CMake's default "lib" prefix makes the installed filename
# (libkwin4_effect_pearostinter.so) disagree with the plugin Id KWin derives
# from it, versus metadata.json's declared Id (kwin4_effect_pearostinter).
# Same bug as pearos-magiclamp: KWin can't reliably resolve the plugin, so
# isEffectSupported()/loadEffect() always fail. Match the other kwin effect
# plugins on this system (e.g. magiclamp.so) by dropping the prefix.
set_target_properties(kwin4_effect_pearostinter PROPERTIES
    PREFIX ""
    AUTORCC ON
)

if(KWIN_DRAWWINDOW_RETURNS_BOOL)
    target_compile_definitions(kwin4_effect_pearostinter
        PRIVATE KWIN_DRAWWINDOW_RETURNS_BOOL)
endif()

target_link_libraries(kwin4_effect_pearostinter
    Qt6::Core
    Qt6::Gui
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::ColorScheme
    KF6::CoreAddons
    KWin::kwin
)

install(
    TARGETS
        kwin4_effect_pearostinter

    DESTINATION
        ${KDE_INSTALL_QTPLUGINDIR}/kwin/effects/plugins/
)
