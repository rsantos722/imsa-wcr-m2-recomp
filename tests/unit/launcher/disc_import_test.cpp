#include "disc_import.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace fs = std::filesystem;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    const auto scratch = fs::temp_directory_path() / ("imsa-import-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(scratch);
        const auto root = launcher_disc::install_root(scratch);
        require(!launcher_disc::installed(root), "missing installation accepted");
        launcher_disc::Progress progress;
        require(!launcher_disc::import_iso(scratch/"missing.iso",root,progress).ok,"missing ISO accepted");
        const auto wrong=scratch/"wrong.iso";
        { std::ofstream f(wrong); f << "not a disc"; }
        require(!launcher_disc::import_iso(wrong,root,progress).ok,"truncated ISO accepted");
        // A same-sized wrong image must fail the digest check. A small expected
        // size stands in for the 671,399,936-byte disc, so the test does not
        // hash a 640 MB file.
        const launcher_disc::IsoSpec small_disc{4096,
            "b34e30635435b13c080e945c6343aa8f6920afc278eef2669970a7632c066835"};
        fs::resize_file(wrong,small_disc.size);
        progress.cancel=true;
        auto canceled_early=launcher_disc::import_iso(wrong,root,progress,small_disc);
        require(!canceled_early.ok && canceled_early.message.find("canceled")!=std::string::npos,
                "cancellation ignored");
        progress.cancel=false;
        auto wrong_digest=launcher_disc::import_iso(wrong,root,progress,small_disc);
        require(!wrong_digest.ok && wrong_digest.message.find("verification failed")!=std::string::npos,
                "wrong ISO digest accepted");
        require(!fs::exists(root),"failed import published game data");
        fs::create_directories(root);
        { std::ofstream f(root/"keep"); f << "existing user data"; }
        require(!launcher_disc::import_iso(wrong,root,progress).ok,"existing data overwritten");
        require(fs::file_size(root/"keep")==18,"existing data changed");
        fs::remove_all(root);
        if (argc==3) {
            // Cancel after extraction has started, not just before validation.
            std::atomic<bool> finished{false};
            std::thread canceler([&] {
                while (!finished && progress.fraction.load() < 0.51f)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!finished) progress.cancel=true;
            });
            progress.fraction=0;
            auto canceled=launcher_disc::import_iso(argv[1],root,progress);
            finished=true;
            canceler.join();
            require(!canceled.ok,"mid-extraction cancellation ignored");
            require(!fs::exists(root),"canceled extraction published installation");
            for (const auto& item:fs::directory_iterator(scratch))
                require(item.path().filename().string().find("game-data.import-")!=0,
                        "canceled import leaked staging files");
            progress.cancel=false;
            const fs::path destination=argv[2];
            auto result=launcher_disc::import_iso(argv[1],destination,progress);
            require(result.ok,result.message.c_str());
            require(launcher_disc::installed(destination),"successful import not detected on restart");
            require(!launcher_disc::import_iso(argv[1],destination,progress).ok,"repeat import overwrote installation");
            std::cout << result.message << '\n';
        }
        fs::remove_all(scratch);
        std::cout << "disc import checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::error_code ec; fs::remove_all(scratch,ec); return 1;
    }
}
