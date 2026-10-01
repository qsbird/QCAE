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
        if (argc != 2 || std::string_view(argv[1]) != "help")
            throw std::runtime_error("Fixed synthetic helper accepts help only");
        require(::fcntl(QCAE_SOLVER_VERSION_FD_CANARY, F_GETFD) == -1 && errno == EBADF,
                "Probe inherited a high host descriptor without close-on-exec");
        return child(std::filesystem::path(argv[0]).filename().string());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
