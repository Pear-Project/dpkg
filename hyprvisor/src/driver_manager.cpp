#include "driver_manager.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <sys/wait.h>

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_GREEN  "\033[32m"
#define C_DIM    "\033[2m"
#define C_RED    "\033[31m"

namespace {

std::string execCmd(const std::string& cmd) {
    std::array<char, 512> buf;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buf.data(), buf.size(), pipe))
        result += buf.data();
    pclose(pipe);
    return result;
}

// Runs a command with its combined stdout+stderr streamed live to std::cout
// (so pkexec/apt progress is visible as it happens), returns the real exit code.
int runStreaming(const std::string& cmd) {
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return -1;
    std::array<char, 512> buf;
    while (fgets(buf.data(), buf.size(), pipe))
        std::cout << buf.data();
    int status = pclose(pipe);
    if (status == -1) return -1;
    return WEXITSTATUS(status);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

// ubuntu-drivers-common is a Recommends, not a hard Depends (it doesn't
// exist on stock Debian at all) -- distinguish "ubuntu-drivers isn't
// installed" from "this GPU genuinely needs no separate driver package",
// which look identical from queryDrivers()' empty result alone.
bool ubuntuDriversAvailable() {
    return !execCmd("command -v ubuntu-drivers 2>/dev/null").empty();
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += c;
        }
    }
    return out;
}

} // namespace

DriverManager::DriverManager(const SystemInfo& info, DriverPreference pref)
    : sysInfo_(info), pref_(pref) {}

bool DriverManager::isInstalled(const std::string& pkg) {
    std::string out = execCmd("dpkg-query -W -f='${Status}' " + pkg + " 2>/dev/null");
    return out.find("install ok installed") != std::string::npos;
}

// Parses `ubuntu-drivers devices` output. Real-world shape:
//   == /sys/devices/pci0000:00/.../0000:01:00.0 ==
//   modalias : pci:v000010DEd...
//   vendor   : NVIDIA Corporation
//   model    : GA104 [GeForce RTX 3070]
//   driver   : nvidia-driver-535 - distro non-free recommended
//   driver   : nvidia-driver-525 - distro non-free
//   driver   : xserver-xorg-video-nouveau - distro free builtin
// AMD/Intel hardware (already handled by in-kernel + mesa on Ubuntu, no
// separate driver package to choose between) simply produces no "driver"
// lines at all -- no vendor branching needed on our side.
std::vector<DriverInfo> DriverManager::queryDrivers() {
    std::string cmd = "ubuntu-drivers devices";
    if (pref_ == DriverPreference::FREE_ONLY) cmd += " --free-only";
    std::string out = execCmd(cmd + " 2>/dev/null");

    std::vector<DriverInfo> result;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        std::string t = trim(line);
        if (t.rfind("driver", 0) != 0) continue;
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string rest = trim(t.substr(colon + 1));

        size_t dash = rest.find(" - ");
        std::string pkg = dash == std::string::npos ? rest : trim(rest.substr(0, dash));
        std::string tag = dash == std::string::npos ? "" : trim(rest.substr(dash + 3));
        if (pkg.empty()) continue;

        bool recommended = tag.find("recommended") != std::string::npos;
        bool openSource  = tag.find("non-free") == std::string::npos &&
                            (tag.find("free") != std::string::npos ||
                             tag.find("builtin") != std::string::npos);

        // Hybrid setups list the same candidate once per PCI device --
        // dedupe by package name, letting any "recommended" tag win.
        auto it = std::find_if(result.begin(), result.end(),
            [&](const DriverInfo& d) { return d.packageName == pkg; });
        if (it != result.end()) {
            it->recommended = it->recommended || recommended;
            continue;
        }

        DriverInfo d;
        d.packageName = pkg;
        d.description = tag;
        d.recommended = recommended;
        d.openSource  = openSource;
        d.installed   = isInstalled(pkg);
        result.push_back(d);
    }
    return result;
}

std::vector<std::string> DriverManager::installedNvidiaPackages() {
    std::string out = execCmd(
        "dpkg-query -W -f='${Package} ${Status}\\n' 2>/dev/null | "
        "grep -E '^(nvidia-|xserver-xorg-video-nvidia|libnvidia-)' | "
        "grep 'install ok installed' | awk '{print $1}'");
    std::vector<std::string> pkgs;
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (!line.empty()) pkgs.push_back(line);
    }
    return pkgs;
}

void DriverManager::printDriverTable(const std::vector<DriverInfo>& drivers) {
    for (const auto& d : drivers) {
        std::cout << "  " << (d.recommended ? C_GREEN "*" C_RESET : " ") << " "
                  << C_BOLD << d.packageName << C_RESET
                  << (d.installed ? C_DIM "  [installed]" C_RESET : "") << "\n"
                  << "      " << d.description << "\n";
    }
    std::cout << "\n" << C_DIM << "  * = recommended\n" << C_RESET;
}

void DriverManager::listDrivers() {
    if (sysInfo_.gpus.empty()) {
        std::cout << C_RED << "[hyprvisor] No GPU detected.\n" << C_RESET;
        return;
    }
    std::cout << C_BOLD << "[hyprvisor] " << sysInfo_.gpus[0].name << C_RESET << "\n";

    if (!ubuntuDriversAvailable()) {
        std::cout << C_RED
            << "  ubuntu-drivers not found -- install ubuntu-drivers-common "
               "for driver management (Ubuntu/derivatives only, not "
               "available on stock Debian).\n" << C_RESET;
        return;
    }

    auto drivers = queryDrivers();
    if (drivers.empty()) {
        std::cout << C_GREEN
            << "  Already fully supported by the in-kernel driver + mesa -- "
               "no separate package to choose.\n" << C_RESET;
        return;
    }
    printDriverTable(drivers);
}

void DriverManager::listDriversJson() {
    auto drivers = queryDrivers();
    std::cout << "{\n"
              << "  \"vmName\": \"" << jsonEscape(sysInfo_.vmName) << "\",\n"
              << "  \"gpus\": [\n";
    for (size_t gi = 0; gi < sysInfo_.gpus.size(); ++gi) {
        const auto& g = sysInfo_.gpus[gi];
        std::cout << "    {\n"
                  << "      \"name\": \"" << jsonEscape(g.name) << "\",\n"
                  << "      \"vendorName\": \"" << jsonEscape(g.vendorName) << "\",\n"
                  << "      \"pciAddr\": \"" << jsonEscape(g.pciAddr) << "\",\n"
                  << "      \"vendorId\": \"" << jsonEscape(g.vendorId) << "\",\n"
                  << "      \"deviceId\": \"" << jsonEscape(g.deviceId) << "\",\n"
                  << "      \"isVirtual\": " << (g.isVirtual ? "true" : "false") << ",\n"
                  << "      \"drivers\": [\n";
        // The driver list isn't per-device from ubuntu-drivers in any
        // reliable way -- attach it to the primary (first) GPU only.
        if (gi == 0) {
            for (size_t i = 0; i < drivers.size(); ++i) {
                const auto& d = drivers[i];
                std::cout << "        {"
                          << "\"package\": \"" << jsonEscape(d.packageName) << "\", "
                          << "\"description\": \"" << jsonEscape(d.description) << "\", "
                          << "\"installed\": " << (d.installed ? "true" : "false") << ", "
                          << "\"recommended\": " << (d.recommended ? "true" : "false") << ", "
                          << "\"openSource\": " << (d.openSource ? "true" : "false") << "}"
                          << (i + 1 < drivers.size() ? "," : "") << "\n";
            }
        }
        std::cout << "      ]\n    }" << (gi + 1 < sysInfo_.gpus.size() ? "," : "") << "\n";
    }
    std::cout << "  ]\n}\n";
}

bool DriverManager::installBestDriver(bool noConfirm) {
    if (!ubuntuDriversAvailable()) {
        std::cerr << C_RED
            << "[hyprvisor] ubuntu-drivers not found -- install "
               "ubuntu-drivers-common for driver management (Ubuntu/"
               "derivatives only, not available on stock Debian).\n"
            << C_RESET;
        return false;
    }

    auto drivers = queryDrivers();
    if (drivers.empty()) {
        std::cout << C_GREEN
            << "[hyprvisor] Nothing to install -- already fully supported by "
               "the in-kernel driver + mesa.\n" << C_RESET;
        return true;
    }

    auto it = std::find_if(drivers.begin(), drivers.end(),
        [](const DriverInfo& d) { return d.recommended; });
    const DriverInfo& pick = (it != drivers.end()) ? *it : drivers.front();

    if (pick.installed) {
        std::cout << C_GREEN << "[hyprvisor] " << pick.packageName
                  << " is already installed.\n" << C_RESET;
        return true;
    }

    std::cout << C_BOLD << "[hyprvisor] Will install: " << C_RESET << pick.packageName
              << C_DIM << "  (" << pick.description << ")\n" << C_RESET;

    if (!noConfirm) {
        std::cout << "Proceed? [y/N] ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            std::cout << "Aborted.\n";
            return false;
        }
    }

    int status = runStreaming("pkexec apt-get install -y " + pick.packageName);
    if (status != 0) {
        std::cerr << C_RED << "[hyprvisor] apt-get exited with code " << status << "\n" << C_RESET;
        return false;
    }
    std::cout << C_GREEN << "[hyprvisor] Installed " << pick.packageName << ".\n" << C_RESET;
    return true;
}

bool DriverManager::cleanAllDrivers(bool noConfirm) {
    auto pkgs = installedNvidiaPackages();
    if (pkgs.empty()) {
        std::cout << C_GREEN << "[hyprvisor] No installed NVIDIA driver packages found.\n" << C_RESET;
        return true;
    }

    std::cout << C_BOLD << "[hyprvisor] Will purge:\n" << C_RESET;
    for (const auto& p : pkgs) std::cout << "  - " << p << "\n";

    if (!noConfirm) {
        std::cout << "Proceed? [y/N] ";
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            std::cout << "Aborted.\n";
            return false;
        }
    }

    std::string cmd = "pkexec apt-get purge -y";
    for (const auto& p : pkgs) cmd += " " + p;
    int status = runStreaming(cmd);
    if (status != 0) {
        std::cerr << C_RED << "[hyprvisor] apt-get exited with code " << status << "\n" << C_RESET;
        return false;
    }
    std::cout << C_GREEN << "[hyprvisor] Purged " << pkgs.size() << " package(s).\n" << C_RESET;
    return true;
}
