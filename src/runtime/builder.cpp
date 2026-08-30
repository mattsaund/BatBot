// SPDX-License-Identifier: MIT
//
// Compiling a ggml backend on demand. See builder.hpp for the shape of it.
#include "batbot/runtime/builder.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>

#include <nlohmann/json.hpp>

#include "batbot/config/paths.hpp"
#include "batbot/runtime/registry.hpp"
#include "batbot/util/format.hpp"

namespace batbot {
namespace {

using json = nlohmann::json;

/// The llama.cpp tag this binary was compiled against. A backend built from a
/// different tag would load and then crash on the first tensor: ggml's
/// internal structures are not stable across releases. CMake passes the tag
/// in so there is exactly one place to change it.
#ifndef BATBOT_LLAMA_TAG
#define BATBOT_LLAMA_TAG "unknown"
#endif

/// Keep the log bounded. A CUDA build emits tens of thousands of lines, and
/// the only ones anybody reads are the last few before it stopped.
constexpr std::size_t kMaxLogLines = 4000;
constexpr std::size_t kTailLines   = 25;

std::string iso_date_now() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
    ::gmtime_r(&now, &parts);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &parts);
    return buffer;
}

/// Pull the percentage out of a cmake/make progress line like
/// "[ 42%] Building CXX object ...". Returns -1 when the line is not one.
int parse_build_percent(const std::string& line, std::string& step) {
    if (line.size() < 4 || line.front() != '[') {
        return -1;
    }
    const std::size_t close = line.find(']');
    if (close == std::string::npos) {
        return -1;
    }
    const std::size_t percent_sign = line.rfind('%', close);
    if (percent_sign == std::string::npos || percent_sign > close) {
        return -1;
    }

    std::string digits = line.substr(1, percent_sign - 1);
    digits.erase(std::remove_if(digits.begin(), digits.end(),
                                [](unsigned char c) { return std::isspace(c) != 0; }),
                 digits.end());
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return -1;
    }

    step = line.size() > close + 2 ? line.substr(close + 2) : std::string();
    // The path is usually far wider than the settings pane; the file name
    // alone is what tells the user the build is moving.
    if (const std::size_t slash = step.rfind('/'); slash != std::string::npos) {
        step = step.substr(slash + 1);
    }
    return std::stoi(digits);
}

/// How many compile jobs to run. Builds are the background task here, not the
/// foreground one, so leave the machine usable: the user is still typing at a
/// TUI while this runs.
std::string job_count() {
    const unsigned int cores = std::max(1U, std::thread::hardware_concurrency());
    return std::to_string(std::max(1U, cores > 2 ? cores - 1 : cores));
}

}  // namespace

std::string BuildProgress::label() const {
    switch (phase) {
        case Phase::Idle:           return "idle";
        case Phase::FetchingSource: return "fetching llama.cpp source";
        case Phase::Configuring:    return "configuring";
        case Phase::Compiling:
            return "compiling " + format::number(static_cast<double>(percent) * 100.0, 0) + "%";
        case Phase::Installing:     return "installing";
        case Phase::Done:           return "installed";
        case Phase::Failed:         return "failed";
        case Phase::Cancelled:      return "cancelled";
    }
    return {};
}

RuntimeBuilder::~RuntimeBuilder() { stop(); }

void RuntimeBuilder::stop() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

BuildProgress RuntimeBuilder::progress() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

bool RuntimeBuilder::start(BackendKind kind, std::function<void()> on_change) {
    if (running_.exchange(true)) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();  // reap the previous, finished, build
    }

    cancel_.store(false);
    on_change_ = std::move(on_change);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_          = BuildProgress{};
        progress_.kind     = kind;
        progress_.phase    = BuildProgress::Phase::Configuring;
        progress_.started  = std::chrono::steady_clock::now();
        progress_.log_file = paths::data_dir() / ("runtime-build-" +
                                                  std::string(backend_info(kind).id) + ".log");
        log_.clear();
    }

    worker_ = std::thread([this, kind] { run(kind); });
    return true;
}

void RuntimeBuilder::cancel() {
    cancel_.store(true);
    const std::lock_guard<std::mutex> lock(child_mutex_);
    if (child_) {
        child_->terminate();
    }
}

void RuntimeBuilder::dismiss() {
    if (running_.load()) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    progress_ = BuildProgress{};
}

void RuntimeBuilder::set_phase(BuildProgress::Phase phase, std::string step) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_.phase = phase;
        progress_.step  = std::move(step);
    }
    if (on_change_) {
        on_change_();
    }
}

void RuntimeBuilder::fail(std::string error) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_.phase = BuildProgress::Phase::Failed;
        progress_.error = std::move(error);
        progress_.log_tail.assign(
            log_.size() > kTailLines ? log_.end() - static_cast<long>(kTailLines) : log_.begin(),
            log_.end());
    }
    if (on_change_) {
        on_change_();
    }
}

bool RuntimeBuilder::run_command(const std::vector<std::string>& argv,
                                 const std::filesystem::path& cwd,
                                 BuildProgress::Phase phase,
                                 bool parse_percent) {
    set_phase(phase);

    auto child = std::make_unique<util::Subprocess>();
    std::string error;
    if (!child->start(argv, cwd, /*extra_env=*/{}, error)) {
        fail(argv[0] + ": " + error);
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(child_mutex_);
        child_ = std::move(child);
    }

    std::string line;
    int last_percent = -1;
    for (;;) {
        util::Subprocess* running = nullptr;
        {
            const std::lock_guard<std::mutex> lock(child_mutex_);
            running = child_.get();
        }
        if (running == nullptr || !running->read_line(line)) {
            break;
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            log_.push_back(line);
            if (log_.size() > kMaxLogLines) {
                log_.erase(log_.begin(), log_.begin() + static_cast<long>(kMaxLogLines / 4));
            }
        }

        if (parse_percent) {
            std::string step;
            const int percent = parse_build_percent(line, step);
            // Redraw on each new percent rather than each line: a build emits
            // thousands of lines and the screen only has a hundred pixels of
            // progress bar to show for them.
            if (percent >= 0 && percent != last_percent) {
                last_percent = percent;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    progress_.percent = static_cast<float>(percent) / 100.0F;
                    progress_.step    = step;
                }
                if (on_change_) {
                    on_change_();
                }
            }
        }
    }

    int status = -1;
    {
        const std::lock_guard<std::mutex> lock(child_mutex_);
        if (child_) {
            status = child_->wait();
            child_.reset();
        }
    }

    // Write the log out whatever happened: a successful build that produced
    // warnings is still worth being able to read afterwards.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream out(progress_.log_file, std::ios::app);
        for (const std::string& entry : log_) {
            out << entry << '\n';
        }
    }

    if (cancel_.load()) {
        return false;
    }
    if (status != 0) {
        fail(argv[0] + " exited with status " + std::to_string(status));
        return false;
    }
    return true;
}

bool RuntimeBuilder::ensure_source(std::string& error) {
    const std::filesystem::path src = paths::runtime_src_dir();
    std::error_code ec;

    if (std::filesystem::exists(src / "CMakeLists.txt", ec)) {
        return true;
    }

    set_phase(BuildProgress::Phase::FetchingSource, "cloning llama.cpp " BATBOT_LLAMA_TAG);
    std::filesystem::create_directories(src.parent_path(), ec);
    std::filesystem::remove_all(src, ec);

    if (!util::on_path("git")) {
        error = "git is needed to fetch the llama.cpp source and is not installed";
        return false;
    }

    const bool ok = run_command({"git", "clone", "--depth", "1", "--branch", BATBOT_LLAMA_TAG,
                                 "https://github.com/ggml-org/llama.cpp.git", src.string()},
                                {}, BuildProgress::Phase::FetchingSource, false);
    if (!ok) {
        error = "could not clone llama.cpp " BATBOT_LLAMA_TAG;
        return false;
    }
    return true;
}

void RuntimeBuilder::run(BackendKind kind) {
    const BackendInfo& info = backend_info(kind);

    // Check the SDK first. A CUDA build that fails on a missing nvcc after
    // four minutes of configuring teaches the user nothing they could not have
    // been told immediately.
    if (!info.required_tool.empty() && !util::on_path(std::string(info.required_tool))) {
        fail(std::string(info.required_tool) + " is not on PATH. Install it with:  sudo apt install " +
             std::string(info.apt_packages));
        running_.store(false);
        return;
    }

    std::string error;
    if (!ensure_source(error)) {
        set_phase(cancel_.load() ? BuildProgress::Phase::Cancelled : BuildProgress::Phase::Failed);
        if (!cancel_.load()) {
            fail(error);
        }
        running_.store(false);
        return;
    }

    const std::filesystem::path src   = paths::runtime_src_dir();
    const std::filesystem::path build = paths::runtime_build_dir() / std::string(info.id);

    std::error_code ec;
    std::filesystem::create_directories(build, ec);

    if (!cancel_.load()) {
        // Only the backend module is wanted, so every other part of llama.cpp
        // is switched off. GGML_CPU is off too: this build exists to produce
        // one .so, and the CPU variants already exist.
        std::vector<std::string> configure = {
            "cmake", "-S", src.string(), "-B", build.string(),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_SHARED_LIBS=ON",
            "-DGGML_BACKEND_DL=ON",
            "-DGGML_NATIVE=OFF",
            "-D" + std::string(info.cmake_option) + "=ON",
            "-DLLAMA_BUILD_TESTS=OFF",
            "-DLLAMA_BUILD_EXAMPLES=OFF",
            "-DLLAMA_BUILD_TOOLS=OFF",
            "-DLLAMA_BUILD_SERVER=OFF",
            "-DLLAMA_BUILD_COMMON=OFF",
            "-DLLAMA_CURL=OFF",
        };
        if (kind == BackendKind::Cpu) {
            // Rebuilding the CPU runtime should produce what shipped: one
            // module per feature level, chosen by score at load time.
            configure.emplace_back("-DGGML_CPU_ALL_VARIANTS=ON");
        } else {
            // A GPU build has no use for the CPU backend, and compiling it
            // would add minutes for a module that already exists.
            configure.emplace_back("-DGGML_CPU=OFF");
        }
        if (!run_command(configure, {}, BuildProgress::Phase::Configuring, false)) {
            if (cancel_.load()) {
                set_phase(BuildProgress::Phase::Cancelled);
            }
            running_.store(false);
            return;
        }
    }

    if (!cancel_.load()) {
        // The target is `ggml`, not the backend itself. ggml makes every
        // enabled backend a dependency of that target, so this builds exactly
        // the modules this configuration turned on -- and it is the only name
        // that works for the CPU backend, which under GGML_CPU_ALL_VARIANTS is
        // a dozen targets (ggml-cpu-haswell, ggml-cpu-zen4, ...) and no single
        // `ggml-cpu` at all.
        const std::vector<std::string> compile = {
            "cmake", "--build", build.string(),
            "--target", "ggml",
            "-j", job_count(),
        };
        if (!run_command(compile, {}, BuildProgress::Phase::Compiling, true)) {
            if (cancel_.load()) {
                set_phase(BuildProgress::Phase::Cancelled);
            }
            running_.store(false);
            return;
        }
    }

    if (cancel_.load()) {
        set_phase(BuildProgress::Phase::Cancelled);
        running_.store(false);
        return;
    }

    // --- install -----------------------------------------------------------
    set_phase(BuildProgress::Phase::Installing);
    const std::filesystem::path target = paths::runtimes_dir();
    std::filesystem::create_directories(target, ec);

    int copied = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(build / "bin", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("libggml-" + std::string(info.id), 0) != 0) {
            continue;
        }
        // copy_file follows the versioned symlinks, so each alias becomes a
        // real file. That is wasteful for a 400 MB CUDA module, so only the
        // module itself is taken -- ggml opens it by that exact name.
        if (entry.path().extension() != ".so") {
            continue;
        }
        std::filesystem::copy_file(entry.path(), target / name,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            fail("could not install " + name + ": " + ec.message());
            running_.store(false);
            return;
        }
        ++copied;
    }

    if (copied == 0) {
        fail("the build produced no " + std::string(info.build_target) + " module");
        running_.store(false);
        return;
    }

    // --- record what was built ---------------------------------------------
    const std::filesystem::path manifest_path = target / "manifest.json";
    json manifest = json::object();
    if (std::ifstream in(manifest_path); in) {
        json parsed = json::parse(in, nullptr, false);
        if (parsed.is_object()) {
            manifest = std::move(parsed);
        }
    }
    manifest[std::string(info.id)] = {
        {"llama_tag", BATBOT_LLAMA_TAG},
        {"built_at", iso_date_now()},
    };
    if (std::ofstream out(manifest_path); out) {
        out << manifest.dump(2) << '\n';
    }

    set_phase(BuildProgress::Phase::Done);
    running_.store(false);
}

}  // namespace batbot
