#include "disc_import.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace launcher_disc {
namespace fs = std::filesystem;
namespace {
constexpr const char* iso_hash = "b34e30635435b13c080e945c6343aa8f6920afc278eef2669970a7632c066835";
constexpr uint64_t iso_size = 671399936;
struct DiscFile { const char* path; uint64_t offset, size; const char* hash; };
#include "launcher_disc_manifest.inc"

// SHA-256 (FIPS 180-4): validate the supported source image and each extracted
// extent without depending on an external executable or platform crypto API.
class Sha256 {
    std::array<uint32_t, 8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                              0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::array<unsigned char,64> block{};
    size_t used = 0;
    uint64_t bytes = 0;
    void compress() {
        static constexpr uint32_t k[] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i=0;i<16;++i) w[i]=uint32_t(block[i*4])<<24 | uint32_t(block[i*4+1])<<16 | uint32_t(block[i*4+2])<<8 | block[i*4+3];
        for (int i=16;i<64;++i) {
            auto x=w[i-15], y=w[i-2];
            w[i]=w[i-16]+(std::rotr(x,7)^std::rotr(x,18)^(x>>3))+w[i-7]+(std::rotr(y,17)^std::rotr(y,19)^(y>>10));
        }
        auto a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
        for (int i=0;i<64;++i) {
            auto t1=v+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            auto t2=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));
            v=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
    }
public:
    void add(const char* data, size_t n) {
        bytes += n;
        for (size_t i=0;i<n;++i) {
            block[used++]=static_cast<unsigned char>(data[i]);
            if (used==64) { compress(); used=0; }
        }
    }
    std::string finish() {
        const auto bits=bytes*8;
        const char pad=char(0x80), zero=0;
        add(&pad,1);
        while (used!=56) add(&zero,1);
        for (int i=7;i>=0;--i) { char c=char(bits>>(i*8)); add(&c,1); }
        std::ostringstream out;
        for (auto word:h) out << std::hex << std::setfill('0') << std::setw(8) << word;
        return out.str();
    }
};
void check_cancel(Progress& p) { if (p.cancel) throw std::runtime_error("Setup canceled."); }
}
fs::path install_root(const fs::path& settings) { return settings / "game-data"; }
bool installed(const fs::path& root) {
    std::ifstream marker(root / ".imsa-disc");
    std::string hash; marker >> hash;
    if (hash != iso_hash) return false;
    std::error_code ec;
    for (const auto& file:disc_files)
        if (!fs::is_regular_file(root/file.path,ec) || fs::file_size(root/file.path,ec)!=file.size || ec) return false;
    return true;
}
Result import_iso(const fs::path& iso, const fs::path& root, Progress& p) {
    return import_iso(iso, root, p, IsoSpec{iso_size, iso_hash});
}
Result import_iso(const fs::path& iso, const fs::path& root, Progress& p, const IsoSpec& expected) {
    fs::path staging;
    try {
        if (fs::exists(root)) throw std::runtime_error("Game data already exists. Restart the launcher to use it.");
        if (fs::file_size(iso)!=expected.size) throw std::runtime_error("Incorrect IMSA Racing ISO (expected 671,399,936 bytes).");
        std::ifstream input(iso,std::ios::binary);
        if (!input) throw std::runtime_error("Could not open the selected ISO.");
        std::vector<char> buffer(1024*1024);
        Sha256 hash;
        for (uint64_t done=0;done<expected.size;) {
            check_cancel(p);
            const auto n=std::min<uint64_t>(buffer.size(),expected.size-done);
            if (!input.read(buffer.data(),n)) throw std::runtime_error("Could not read the complete ISO.");
            hash.add(buffer.data(),n); done+=n; p.fraction=0.5f*float(done)/expected.size;
        }
        if (hash.finish()!=expected.sha256) throw std::runtime_error("ISO verification failed.");
        fs::create_directories(root.parent_path());
        fs::path candidate=root;
        candidate+=".import-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        if (!fs::create_directory(candidate)) throw std::runtime_error("Could not create the setup staging folder.");
        staging=candidate;
        size_t count=0;
        for (const auto& file:disc_files) {
            check_cancel(p);
            auto dest=staging/file.path;
            fs::create_directories(dest.parent_path());
            std::ofstream out(dest,std::ios::binary);
            input.clear(); input.seekg(file.offset);
            Sha256 file_hash;
            for (uint64_t left=file.size;left;) {
                check_cancel(p);
                const auto n=std::min<uint64_t>(left,buffer.size());
                if (!input.read(buffer.data(),n) || !out.write(buffer.data(),n)) throw std::runtime_error("Could not extract game data. Check available disk space and permissions.");
                file_hash.add(buffer.data(),n); left-=n;
            }
            out.close();
            if (!out || file_hash.finish()!=file.hash) throw std::runtime_error("Extracted file verification failed: "+std::string(file.path));
            p.fraction=0.5f+0.5f*float(++count)/std::size(disc_files);
        }
        std::ofstream marker(staging/".imsa-disc"); marker << iso_hash << '\n'; marker.close();
        if (!marker) throw std::runtime_error("Could not save installation receipt.");
        check_cancel(p);
        // Never merge with a partial installation or overwrite an existing one.
        if (fs::exists(root)) throw std::runtime_error("Game data already exists. Restart the launcher to use it.");
        fs::rename(staging,root); staging.clear();
        return {true,"Setup complete."};
    } catch (const std::exception& e) {
        if (!staging.empty()) { std::error_code ec; fs::remove_all(staging,ec); }
        return {false,e.what()};
    }
}
}
