#pragma once
#include "gpu_detect.hpp"
#include <string>
#include <vector>

// Debian/Ubuntu edition: no per-GPU-generation package database (that lived
// entirely in Arch/AUR package names). ubuntu-drivers-common already knows
// which NVIDIA driver package fits the detected hardware -- this class is a
// thin wrapper around `ubuntu-drivers devices`/`install`, not a from-scratch
// recipe book. AMD/Intel need no separate driver package on Ubuntu (mesa,
// already installed, handles them), so `ubuntu-drivers devices` simply
// returns nothing for those -- no vendor branching needed here at all.
enum class DriverPreference {
    AUTO,      // whatever ubuntu-drivers recommends (may be proprietary)
    FREE_ONLY  // only consider free/open-source packages (--free-only)
};

struct DriverInfo {
    std::string packageName;
    std::string description;  // raw tag from `ubuntu-drivers devices`, e.g. "distro non-free recommended"
    bool        installed;
    bool        recommended;
    bool        openSource;   // tag contains "free" (and not "non-free") or "builtin"
};

class DriverManager {
public:
    DriverManager(const SystemInfo& info, DriverPreference pref = DriverPreference::AUTO);

    void listDrivers();
    void listDriversJson();
    bool installBestDriver(bool noConfirm = false);
    // Purges every installed nvidia-* driver package for a clean slate
    // before a fresh --install. Prompts for confirmation unless noConfirm.
    bool cleanAllDrivers(bool noConfirm = false);

private:
    SystemInfo       sysInfo_;
    DriverPreference pref_;

    std::vector<DriverInfo> queryDrivers();
    bool isInstalled(const std::string& pkg);
    std::vector<std::string> installedNvidiaPackages();
    void printDriverTable(const std::vector<DriverInfo>& drivers);
};
