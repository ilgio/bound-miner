// Milestone 12: bound_fuzz, which runs the fuzz targets (see fuzz.h).
#include "fuzz/fuzz.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace bound::fuzz {

std::map<std::string, Target>& registry() {
    static std::map<std::string, Target> targets;
    return targets;
}

void fail(const char* what) {
    std::fprintf(stderr, "fuzz check failed: %s\n", what);
    std::abort();
}

}  // namespace bound::fuzz

using bound::Bytes;
using bound::fuzz::registry;
using bound::fuzz::Target;

#ifdef BOUND_LIBFUZZER

// libFuzzer: one target per process, named by FUZZ.
namespace {
Target g_target;
}

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    const char* name = std::getenv("FUZZ");
    if (name != nullptr && std::string(name) == "list") {  // FUZZ=list: the targets' names, one a line
        for (const auto& [n, t] : registry()) std::cout << n << "\n";
        std::exit(0);
    }
    const auto found = name != nullptr ? registry().find(name) : registry().end();
    if (found == registry().end()) {
        std::cerr << "Set FUZZ to one of:";
        for (const auto& [n, t] : registry()) std::cerr << " " << n;
        std::cerr << "\n";
        std::exit(2);
    }
    g_target = found->second;
    // FUZZ_WRITE_SEEDS=<dir>: write the target's seeds there as a starting corpus, and stop.
    if (const char* dir = std::getenv("FUZZ_WRITE_SEEDS")) {
        std::filesystem::create_directories(dir);
        const std::vector<Bytes> seeds = g_target.seeds != nullptr ? g_target.seeds() : std::vector<Bytes>{};
        for (std::size_t i = 0; i < seeds.size(); ++i) {
            std::ofstream(std::filesystem::path(dir) / ("seed-" + std::to_string(i)), std::ios::binary)
                .write(reinterpret_cast<const char*>(seeds[i].data()), static_cast<std::streamsize>(seeds[i].size()));
        }
        std::exit(0);
    }
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    g_target.run({data, size});
    return 0;
}

#else

// Without libFuzzer: random mutations of each target's seeds, so every target runs under any
// compiler (and in ctest). No coverage feedback; the libFuzzer build is for finding things.
namespace {

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {  // xorshift64*
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1DULL;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

Bytes mutate(Bytes b, const std::vector<Bytes>& corpus, Rng& rng) {
    const std::size_t edits = 1 + rng.below(8);
    for (std::size_t e = 0; e < edits; ++e) {
        switch (rng.below(8)) {
        case 0:  // flip a bit
            if (!b.empty()) b[rng.below(b.size())] ^= static_cast<std::uint8_t>(1u << rng.below(8));
            break;
        case 1:  // set a byte to an interesting value
            if (!b.empty()) {
                const std::uint8_t values[] = {0x00, 0x01, 0x7f, 0x80, 0xfd, 0xfe, 0xff};
                b[rng.below(b.size())] = values[rng.below(std::size(values))];
            }
            break;
        case 2:  // insert random bytes
            for (std::size_t i = 0, n = 1 + rng.below(16); i < n; ++i) {
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(rng.below(b.size() + 1)), static_cast<std::uint8_t>(rng.next()));
            }
            break;
        case 3:  // delete a run
            if (!b.empty()) {
                const std::size_t at = rng.below(b.size());
                const std::size_t n = 1 + rng.below(std::min<std::size_t>(16, b.size() - at));
                b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
            }
            break;
        case 4:  // truncate
            b.resize(rng.below(b.size() + 1));
            break;
        case 5:  // duplicate a run
            if (!b.empty() && b.size() < 32768) {
                const std::size_t at = rng.below(b.size());
                const std::size_t n = 1 + rng.below(std::min<std::size_t>(64, b.size() - at));
                const Bytes run(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(rng.below(b.size() + 1)), run.begin(), run.end());
            }
            break;
        case 6:  // splice another seed in
            if (!corpus.empty()) {
                const Bytes& other = corpus[rng.below(corpus.size())];
                const std::size_t at = rng.below(b.size() + 1);
                b.resize(at);
                b.insert(b.end(), other.begin() + static_cast<std::ptrdiff_t>(rng.below(other.size() + 1)), other.end());
            }
            break;
        default:  // overwrite with a random 32-bit value
            if (b.size() >= 4) {
                const std::size_t at = rng.below(b.size() - 3);
                const std::uint32_t v = static_cast<std::uint32_t>(rng.next());
                for (int i = 0; i < 4; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
            }
            break;
        }
    }
    return b;
}

// Runs one input; on an escaped exception, saves the input for reproducing and says so.
bool run_one(const std::string& name, const Target& target, const Bytes& input) {
    try {
        target.run(input);
        return true;
    } catch (const std::exception& e) {
        const std::string file = "crash-" + name + "-" + std::to_string(std::hash<std::string>{}(std::string(input.begin(), input.end())));
        std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size()));
        std::cerr << name << ": an exception escaped: " << e.what() << " (input saved to " << file << ")\n";
        return false;
    }
}

int usage() {
    std::cerr << "usage: bound_fuzz <target|all> [--runs <n>] [--seed <n>] [input files...]\n       targets:";
    for (const auto& [n, t] : registry()) std::cerr << " " << n;
    std::cerr << "\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    const std::string which = args[0];
    std::size_t runs = 2000;
    std::uint64_t seed = 1;
    std::vector<std::filesystem::path> files;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--runs" && i + 1 < args.size()) {
            runs = std::stoul(args[++i]);
        } else if (args[i] == "--seed" && i + 1 < args.size()) {
            seed = std::stoull(args[++i]);
        } else {
            files.emplace_back(args[i]);
        }
    }
    if (which != "all" && !registry().contains(which)) return usage();

    bool ok = true;
    for (const auto& [name, target] : registry()) {
        if (which != "all" && name != which) continue;
        if (!files.empty()) {
            for (const auto& f : files) {
                std::ifstream in(f, std::ios::binary);
                const Bytes input{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                ok = run_one(name, target, input) && ok;
            }
            std::cout << name << ": " << files.size() << " files\n";
            continue;
        }
        std::vector<Bytes> corpus = target.seeds != nullptr ? target.seeds() : std::vector<Bytes>{};
        for (const Bytes& s : corpus) ok = run_one(name, target, s) && ok;  // the seeds themselves
        if (corpus.empty()) corpus.push_back({});
        Rng rng{seed * 0x9E3779B97F4A7C15ULL + std::hash<std::string>{}(name) + 1};
        std::size_t done = 0;
        for (; done < runs && ok; ++done) ok = run_one(name, target, mutate(corpus[rng.below(corpus.size())], corpus, rng)) && ok;
        std::cout << name << ": " << done << " runs" << (ok ? "" : ", FAILED") << "\n";
        if (!ok) break;
    }
    return ok ? 0 : 1;
}

#endif
