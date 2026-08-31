// SPDX-License-Identifier: MIT
//
// See resources.hpp.
#include "batbot/util/resources.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "batbot/util/subprocess.hpp"

namespace batbot::util {
namespace {

/// How often the machine is read.
///
/// Set by what the reading costs rather than by what would be nice. Starting
/// nvidia-smi means fork(), and fork() copies the page tables of a process that
/// has a model mapped into it: measured at 0.4 ms with nothing loaded and
/// 6.9 ms with an 11 GB expert resident, rising with the model. Two and a half
/// seconds keeps that under a third of a percent of one core while still being
/// often enough to watch video memory fill during a load.
///
/// Reading the memory from ggml instead would cost nothing at all, and is the
/// obvious improvement -- except that ggml's device registry is written by the
/// engine thread as backends are loaded, and reading it from here would be a
/// race. A separate process has no such problem, which is most of why this one
/// is a separate process.
constexpr auto kInterval = std::chrono::milliseconds(2500);

constexpr std::uint64_t kKib = 1024;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return text;
}

/// A whole number out of `text`, or nothing.
bool to_number(std::string_view text, std::uint64_t& value) {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// The processor package temperature, in whole degrees, or -1.
///
/// hwmon first, because that is where a modern kernel puts it and the name
/// says which sensor it is. The thermal zones are the fallback for kernels or
/// platforms that only expose it there.
int cpu_temperature() {
    static constexpr std::array<std::string_view, 4> kWanted{
        {"coretemp", "k10temp", "zenpower", "cpu_thermal"}};

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/hwmon", ec)) {
        const std::string name = std::string(trim(read_file(entry.path() / "name")));
        if (std::find(kWanted.begin(), kWanted.end(), name) == kWanted.end()) {
            continue;
        }
        std::uint64_t milli = 0;
        if (to_number(read_file(entry.path() / "temp1_input"), milli)) {
            return static_cast<int>(milli / 1000);
        }
    }

    ec.clear();
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/thermal", ec)) {
        if (trim(read_file(entry.path() / "type")) != "x86_pkg_temp") {
            continue;
        }
        std::uint64_t milli = 0;
        if (to_number(read_file(entry.path() / "temp"), milli)) {
            return static_cast<int>(milli / 1000);
        }
    }
    return -1;
}

/// Every GPU nvidia-smi can see. Empty on a machine without it, which is not an
/// error -- it is a machine with no NVIDIA card.
std::vector<ResourceSample> read_gpus() {
    std::vector<ResourceSample> gpus;
    if (!on_path("nvidia-smi")) {
        return gpus;
    }
    Subprocess child;
    std::string error;
    if (!child.start({"nvidia-smi",
                      "--query-gpu=name,memory.used,memory.total,temperature.gpu,utilization.gpu",
                      "--format=csv,noheader,nounits"},
                     {}, /*extra_env=*/{}, error)) {
        return gpus;
    }
    std::string line;
    while (child.read_line(line)) {
        ResourceSample sample;
        if (parse_gpu_line(line, sample)) {
            gpus.push_back(std::move(sample));
        }
    }
    child.wait();
    return gpus;
}

}  // namespace

int ResourceSample::memory_percent() const {
    if (total == 0) {
        return -1;
    }
    return static_cast<int>((used * 100 + total / 2) / total);
}

bool parse_gpu_line(std::string_view line, ResourceSample& into) {
    std::array<std::string_view, 5> fields{};
    std::size_t count = 0;
    std::size_t at    = 0;
    while (count < fields.size() && at <= line.size()) {
        const std::size_t comma = line.find(',', at);
        fields[count++] = trim(line.substr(at, comma == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : comma - at));
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 1;
    }
    if (count < 3 || fields[0].empty()) {
        return false;
    }

    std::uint64_t used_mb  = 0;
    std::uint64_t total_mb = 0;
    if (!to_number(fields[1], used_mb) || !to_number(fields[2], total_mb)) {
        return false;
    }

    into.name  = std::string(fields[0]);
    into.used  = used_mb * kKib * kKib;
    into.total = total_mb * kKib * kKib;

    // "[N/A]" for either of these on a card that does not report it, which is
    // a fact about the card rather than a parse failure.
    std::uint64_t number = 0;
    into.temperature_c = count > 3 && to_number(fields[3], number) ? static_cast<int>(number) : -1;
    into.busy_percent  = count > 4 && to_number(fields[4], number) ? static_cast<int>(number) : -1;
    return true;
}

bool parse_meminfo(std::string_view text, std::uint64_t& used, std::uint64_t& total) {
    std::uint64_t mem_total     = 0;
    std::uint64_t mem_available = 0;

    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line =
            text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? text.size() + 1 : end + 1;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, colon);
        std::string_view value = trim(line.substr(colon + 1));
        // "16384 kB" -- the unit is always kB where it is present at all.
        if (const std::size_t space = value.find(' '); space != std::string_view::npos) {
            value = value.substr(0, space);
        }
        std::uint64_t number = 0;
        if (!to_number(value, number)) {
            continue;
        }
        if (key == "MemTotal")     { mem_total     = number * kKib; }
        if (key == "MemAvailable") { mem_available = number * kKib; }
    }

    if (mem_total == 0) {
        return false;
    }
    total = mem_total;
    used  = mem_available < mem_total ? mem_total - mem_available : 0;
    return true;
}

bool parse_stat(std::string_view text, std::uint64_t& busy, std::uint64_t& total) {
    const std::size_t end = text.find('\n');
    const std::string_view line =
        text.substr(0, end == std::string_view::npos ? std::string_view::npos : end);
    if (line.substr(0, 4) != "cpu ") {
        return false;
    }

    // user nice system idle iowait irq softirq steal guest guest_nice.
    // Idle and iowait are the two that are not work.
    std::uint64_t sum  = 0;
    std::uint64_t idle = 0;
    std::size_t   field = 0;
    std::size_t   at    = 4;
    while (at < line.size()) {
        while (at < line.size() && line[at] == ' ') {
            ++at;
        }
        const std::size_t space = line.find(' ', at);
        const std::string_view value =
            line.substr(at, space == std::string_view::npos ? std::string_view::npos : space - at);
        std::uint64_t number = 0;
        if (!value.empty() && to_number(value, number)) {
            sum += number;
            if (field == 3 || field == 4) {
                idle += number;
            }
            ++field;
        }
        if (space == std::string_view::npos) {
            break;
        }
        at = space + 1;
    }
    if (field < 4) {
        return false;
    }
    total = sum;
    busy  = sum - idle;
    return true;
}

std::string parse_cpu_name(std::string_view cpuinfo) {
    std::size_t at = 0;
    while (at <= cpuinfo.size()) {
        const std::size_t end = cpuinfo.find('\n', at);
        const std::string_view line =
            cpuinfo.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? cpuinfo.size() + 1 : end + 1;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || trim(line.substr(0, colon)) != "model name") {
            continue;
        }

        std::string name(trim(line.substr(colon + 1)));
        // The trademark furniture is not a label. "12th Gen Intel(R) Core(TM)
        // i5-12400" is thirty-four columns of which nine identify the part.
        for (const std::string_view noise : {"(R)", "(TM)", "(tm)", "CPU", "Processor"}) {
            for (std::size_t found = name.find(noise); found != std::string::npos;
                 found = name.find(noise)) {
                name.erase(found, noise.size());
            }
        }
        if (const std::size_t at_sign = name.find(" @ "); at_sign != std::string::npos) {
            name.erase(at_sign);  // the clock is not the name, and it is not the clock either
        }
        // Collapse the gaps the removals left.
        std::string tidy;
        bool spaced = true;
        for (const char c : name) {
            if (c == ' ') {
                if (!spaced) {
                    tidy += ' ';
                }
                spaced = true;
                continue;
            }
            tidy += c;
            spaced = false;
        }
        return std::string(trim(tidy));
    }
    return {};
}

ResourceMonitor::~ResourceMonitor() { stop(); }

void ResourceMonitor::start(std::function<void()> on_change) {
    if (running_.exchange(true)) {
        return;
    }
    on_change_ = std::move(on_change);
    worker_    = std::thread([this] { run(); });
}

void ResourceMonitor::stop() {
    running_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }
}

ResourceSnapshot ResourceMonitor::snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void ResourceMonitor::run() {
    const std::string cpu_name = parse_cpu_name(read_file("/proc/cpuinfo"));

    while (running_.load()) {
        ResourceSnapshot next;
        next.ready    = true;
        next.gpus     = read_gpus();
        next.cpu.name = cpu_name.empty() ? "processor" : cpu_name;

        parse_meminfo(read_file("/proc/meminfo"), next.cpu.used, next.cpu.total);
        next.cpu.temperature_c = cpu_temperature();

        // A percentage of processor time needs two readings, so the first pass
        // reports none rather than reporting the average since boot.
        std::uint64_t busy  = 0;
        std::uint64_t total = 0;
        if (parse_stat(read_file("/proc/stat"), busy, total) && total > last_total_) {
            if (last_total_ != 0) {
                const std::uint64_t span = total - last_total_;
                next.cpu.busy_percent =
                    static_cast<int>(((busy - last_busy_) * 100 + span / 2) / span);
            }
            last_busy_  = busy;
            last_total_ = total;
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            latest_ = std::move(next);
        }
        if (on_change_) {
            on_change_();
        }

        // Woken in short steps so that quitting does not wait out the interval.
        for (int slept = 0; slept < 25 && running_.load(); ++slept) {
            std::this_thread::sleep_for(kInterval / 25);
        }
    }
}

}  // namespace batbot::util
