cmake_minimum_required(VERSION 3.16)
project(pearos-tinter)

set(KF_MIN_VERSION "6.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH
    ${CMAKE_MODULE_PATH}
    ${ECM_MODULE_PATH}
)

include(FeatureSummary)
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
)

find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
    Config
    ColorScheme
    ConfigWidgets
    CoreAddons
)

find_package(KWin REQUIRED)
find_package(KWinDBusInterface CONFIG REQUIRED)

# KWin changed OffscreenEffect::drawWindow() to return bool (whether the
# window was actually painted) instead of void. KWIN_DRAWWINDOW_RETURNS_BOOL
# is set by this probe to match the installed KWin so `override` stays
# correct on both old and new KWin versions. Same issue pearos-magiclamp
# probes for as KWIN_PAINTWINDOW_RETURNS_BOOL, on a different virtual.
file(WRITE "${CMAKE_BINARY_DIR}/kwin_drawwindow_probe.cc"
"#include <effect/offscreeneffect.h>
struct Probe : KWin::OffscreenEffect {
    bool drawWindow(const KWin::RenderTarget&, const KWin::RenderViewport&, KWin::EffectWindow*, int, const KWin::Region&, KWin::WindowPaintData&) override { return true; }
};
int main() { return 0; }
")
try_compile(KWIN_DRAWWINDOW_RETURNS_BOOL
    SOURCES "${CMAKE_BINARY_DIR}/kwin_drawwindow_probe.cc"
    LINK_LIBRARIES KWin::kwin Qt6::Core Qt6::Gui
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)

add_subdirectory(src)

feature_summary(WHAT ALL)
