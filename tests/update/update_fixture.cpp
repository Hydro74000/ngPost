// A native executable keeps Windows file locks real and probes/restarts observable.
// It has no Qt dependency; release validation additionally uses the actual packages.
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char **argv)
{
    auto root = std::filesystem::absolute(argv[0]).parent_path();
#ifdef __APPLE__
    root = root.parent_path().parent_path();
#endif
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--hold") {
        while (!std::filesystem::exists(root / "exit"))
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return 0;
    }
    if (mode == "--version") {
        if (std::filesystem::exists(root / "fail-probe")
            || (root.filename() == "installed"
                && std::filesystem::exists(root / "fail-after-swap")))
            return 1;
        return 0;
    }
    std::ofstream(root / "restarted") << "new process";
    return 0;
}
