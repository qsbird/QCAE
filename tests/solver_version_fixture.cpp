#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int child(std::string_view mode) {
    require(std::getenv("QCAE_VERSION_TEST_CANARY") == nullptr,
            "Probe inherited test-only host canary environment");
    require(std::getenv("LANG") != nullptr && std::string_view(std::getenv("LANG")) == "C" &&
                std::getenv("LC_ALL") != nullptr &&
                std::string_view(std::getenv("LC_ALL")) == "C" &&
                std::filesystem::current_path().filename().string().starts_with(".qcae-version-"),
            "Probe did not use its fixed locale and private working directory");
    std::cout << "QCAE SYNTHETIC VERSION PROBE\n";
    if (mode == "synthetic-descendant") {
        int barrier[2];
        require(::pipe(barrier) == 0, "Synthetic descendant barrier failed");
        const auto descendant = ::fork();
        require(descendant >= 0, "Synthetic descendant fork failed");
        if (descendant == 0) {
            ::close(barrier[0]);
            ::close(STDOUT_FILENO);
            ::close(STDERR_FILENO);
            (void)::write(barrier[1], "Y", 1);
            ::close(barrier[1]);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            std::ofstream("descendant.finished") << "Explicit bounded test-only descendant";
            ::_exit(0);
        }
        ::close(barrier[1]);
        char ready{};
        require(::read(barrier[0], &ready, 1) == 1 && ready == 'Y',
                "Synthetic descendant did not reach its real barrier");
        ::close(barrier[0]);
    }
    if (mode == "synthetic-timeout") {
        std::signal(SIGTERM, SIG_IGN);
        for (;;)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (mode == "synthetic-output") {
        std::cout << std::string(70000, 'x') << std::flush;
        return 0;
    }
    if (mode == "synthetic-aggregate-output") {
        std::cout << std::string(40000, 'x') << std::flush;
        std::cerr << std::string(30000, 'y') << std::flush;
        return 0;
    }
    if (mode.starts_with("synthetic-mystran-")) {
        constexpr std::string_view banner =
            " MYSTRAN Version 19.0.0   JUN 28 2026 MYSTRAN developed by Dr Bill Case\n";
        if (mode == "synthetic-mystran-noheader")
            std::cout << "Compatible solver 19.0.0\n";
        else if (mode == "synthetic-mystran-substring")
            std::cout << "Advertisement MYSTRAN Version 19.0.0\n";
        else if (mode == "synthetic-mystran-word")
            std::cout << "MYSTRANX Version 19.0.0\n";
        else if (mode == "synthetic-mystran-token")
            std::cout << "MYSTRAN Version 19.0.0X\n";
        else if (mode == "synthetic-mystran-patch")
            std::cout << "MYSTRAN Version 19.0.0.1\n";
        else if (mode == "synthetic-mystran-old")
            std::cout << "MYSTRAN Version 18.0.0\n";
        else if (mode == "synthetic-mystran-prefix")
            std::cout << "MYSTRAN Version19.0.0\n";
        else if (mode == "synthetic-mystran-vprefix")
            std::cout << "MYSTRAN Version V19.0.0\n";
        else if (mode == "synthetic-mystran-wrong-vendor")
            std::cout << "MSC Nastran 2024.1\n";
        else if (mode == "synthetic-mystran-many")
            std::cout << banner << banner;
        else if (mode == "synthetic-mystran-versions")
            std::cout << banner << "MYSTRAN Version 18.0.0\n";
        else if (mode == "synthetic-mystran-mixed")
            std::cout << banner << "MSC Nastran 2024.1\n";
        else if (mode == "synthetic-mystran-stderr-conflict") {
            std::cout << banner;
            std::cerr << "MSC Nastran V2022.1\n";
        } else if (mode == "synthetic-mystran-stderr")
            std::cerr << "\tMYSTRAN Version 19.0.0\r\n";
        else {
            std::cout << banner;
            if (mode == "synthetic-mystran-nul")
                std::cout.put('\0');
        }
        std::cerr << "Explicit test-only version output, not an installed MYSTRAN\n";
        return mode == "synthetic-mystran-nonzero" ? 7 : 0;
    }
    if (mode == "synthetic-noheader")
        std::cout << "Compatible solver 2024.1\n";
    else if (mode == "synthetic-substring")
        std::cout << "Advertisement MSC Nastran 2024.1\n";
    else if (mode == "synthetic-many")
        std::cout << "MSC Nastran 2024.1\nMSC Nastran 2022.1\n";
    else if (mode == "synthetic-patch")
        std::cout << "MSC Nastran 2024.1.9\n";
    else if (mode == "synthetic-2022")
        std::cout << "MSC Nastran V2022.1\n";
    else if (mode == "synthetic-nul")
        std::cout << std::string("MSC Nastran 2024.1\n\0", 20);
    else
        std::cout << "MSC Nastran 2024.1\n";
    std::cerr << "Explicit test-only version output, not an installed Nastran\n";
    return mode == "synthetic-nonzero" ? 7 : 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto mode = std::filesystem::path(argv[0]).filename().string();
        const auto argument = mode.starts_with("synthetic-mystran-") ? "--version" : "help";
        if (argc != 2 || std::string_view(argv[1]) != argument)
            throw std::runtime_error("Fixed synthetic helper received an unexpected argument");
        require(::fcntl(QCAE_SOLVER_VERSION_FD_CANARY, F_GETFD) == -1 && errno == EBADF,
                "Probe inherited a high host descriptor without close-on-exec");
        return child(mode);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
