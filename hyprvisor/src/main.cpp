#include "driver_manager.hpp"
#include "gpu_detect.hpp"
#include <iostream>
#include <string>

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_CYAN   "\033[36m"
#define C_YELLOW "\033[33m"
#define C_RED    "\033[31m"
#define C_DIM    "\033[2m"

static void printBanner() {
    std::cout << C_CYAN C_BOLD
              << "  _               _\n"
                 " | |__  _   _ _ _| |__  _ __ __   ___  ___  _ __\n"
                 " | '_ \\| | | | '_ \\ '__| '_ \\ \\ / / |/ __|| '__|\n"
                 " | | | | |_| | |_) | |  | | | \\ V /| |\\__ \\| |\n"
                 " |_| |_|\\__, | .__/|_|  |_| |_|\\_/ |_||___/|_|\n"
                 "        |___/|_|   GPU Driver Manager\n"
              << C_RESET << "\n";
}

static void printUsage(const char* prog) {
    std::cout
        << C_BOLD << "Usage:\n" << C_RESET
        << "  " << prog << " " C_CYAN "--detect" C_RESET
        << "\n      Detect GPU(s) and current drivers\n\n"
        << "  " << prog << " " C_CYAN "--list" C_RESET
        << " [--free-only] [--json]\n"
        << "      List available driver packages, as reported by ubuntu-drivers\n"
        << "      (--json for machine-readable output)\n\n"
        << "  " << prog << " " C_CYAN "--clean" C_RESET " [--noconfirm]\n"
        << "      Purge every installed NVIDIA driver package for a clean slate.\n"
        << "      Prompts for confirmation unless --noconfirm is given. Chain with\n"
        << "      --install:\n"
        << "        " << prog << " --clean --noconfirm && " << prog << " --install --noconfirm\n\n"
        << "  " << prog << " " C_CYAN "--install" C_RESET " [--free-only] [--noconfirm]\n"
        << "      Install ubuntu-drivers' recommended package via pkexec apt-get.\n"
        << "      --noconfirm skips the confirmation prompt.\n\n"
        << C_BOLD << "Options:\n" << C_RESET
        << "  --free-only   Only consider free/open-source driver packages\n"
        << C_DIM
        << "  (default)      NVIDIA  -> whatever ubuntu-drivers marks recommended\n"
           "                            (usually the proprietary nvidia-driver-XXX)\n"
           "                  AMD/Intel -> nothing to choose, already handled by\n"
           "                            the in-kernel driver + mesa\n"
        << C_RESET << "\n";
}

static void printDetect(const SystemInfo& info) {
    if (!info.vmName.empty()) {
        std::cout << C_YELLOW C_BOLD
                  << "[hyprvisor] Virtualized environment: " << info.vmName
                  << C_RESET << "\n";
    }

    if (info.gpus.empty()) {
        std::cout << C_RED << "[hyprvisor] No GPU detected.\n" << C_RESET;
        return;
    }

    std::cout << C_BOLD << "[hyprvisor] Detected GPU(s):\n" << C_RESET;
    int idx = 1;
    for (const auto& g : info.gpus) {
        std::cout << "\n  " C_BOLD << idx++ << ". " << g.name << C_RESET;
        if (g.isVirtual)
            std::cout << C_YELLOW << "  [Virtual GPU]" << C_RESET;
        std::cout << "\n";
        std::cout << "     Vendor  : " << g.vendorName << "\n"
                  << "     PCI addr: " << g.pciAddr    << "\n"
                  << "     IDs     : " << g.vendorId << ":" << g.deviceId << "\n";
    }
    std::cout << "\n";
}

static DriverPreference parsePreference(int argc, char* argv[], int start) {
    for (int i = start; i < argc; ++i)
        if (std::string(argv[i]) == "--free-only")
            return DriverPreference::FREE_ONLY;
    return DriverPreference::AUTO;
}

int main(int argc, char* argv[]) {
    bool jsonMode = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--json") jsonMode = true;

    if (!jsonMode) printBanner();

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string flag = argv[1];

    if (flag == "--help" || flag == "-h") {
        printUsage(argv[0]);
        return 0;
    }

    if (flag == "--detect") {
        GPUDetector detector;
        SystemInfo  info = detector.detect();
        printDetect(info);
        return 0;
    }

    if (flag == "--list") {
        DriverPreference pref = parsePreference(argc, argv, 2);
        bool wantJson = false;
        for (int i = 2; i < argc; ++i)
            if (std::string(argv[i]) == "--json") wantJson = true;

        GPUDetector   detector;
        SystemInfo    info = detector.detect();
        DriverManager mgr(info, pref);
        if (wantJson) mgr.listDriversJson();
        else          mgr.listDrivers();
        return 0;
    }

    if (flag == "--clean") {
        bool noConfirm = false;
        for (int i = 2; i < argc; ++i)
            if (std::string(argv[i]) == "--noconfirm") noConfirm = true;

        GPUDetector   detector;
        SystemInfo    info = detector.detect();
        DriverManager mgr(info, DriverPreference::AUTO);
        bool ok = mgr.cleanAllDrivers(noConfirm);
        return ok ? 0 : 1;
    }

    if (flag == "--install") {
        DriverPreference pref = parsePreference(argc, argv, 2);
        bool noConfirm = false;
        for (int i = 2; i < argc; ++i)
            if (std::string(argv[i]) == "--noconfirm") noConfirm = true;

        if (noConfirm)
            std::cout << C_YELLOW
                      << "[hyprvisor] --noconfirm: installing unattended, no prompt.\n"
                      << C_RESET;

        GPUDetector   detector;
        SystemInfo    info = detector.detect();
        printDetect(info);
        DriverManager mgr(info, pref);
        bool ok = mgr.installBestDriver(noConfirm);
        return ok ? 0 : 1;
    }

    std::cerr << C_RED << "[hyprvisor] Unknown option: " << flag << C_RESET << "\n\n";
    printUsage(argv[0]);
    return 1;
}
