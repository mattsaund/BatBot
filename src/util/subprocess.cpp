// SPDX-License-Identifier: MIT
//
// fork/exec with a merged output pipe. See subprocess.hpp for why not popen.
#include "crucible/util/subprocess.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace crucible::util {
namespace {

/// A NULL-terminated char* array pointing into strings we keep alive.
/// execvp wants char* const*, and string::data() is writable since C++17.
std::vector<char*> to_argv(const std::vector<std::string>& args) {
    std::vector<char*> raw;
    raw.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        raw.push_back(const_cast<char*>(arg.c_str()));
    }
    raw.push_back(nullptr);
    return raw;
}

}  // namespace

Subprocess::~Subprocess() {
    if (running()) {
        terminate();
        wait();
    }
    close_pipe();
}

Subprocess::Subprocess(Subprocess&& other) noexcept { *this = std::move(other); }

Subprocess& Subprocess::operator=(Subprocess&& other) noexcept {
    if (this != &other) {
        close_pipe();
        pid_    = std::exchange(other.pid_, -1);
        fd_     = std::exchange(other.fd_, -1);
        status_ = std::exchange(other.status_, -1);
        reaped_ = std::exchange(other.reaped_, false);
        buffer_ = std::move(other.buffer_);
        eof_    = std::exchange(other.eof_, false);
    }
    return *this;
}

void Subprocess::close_pipe() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Subprocess::start(const std::vector<std::string>& argv,
                       const std::filesystem::path& cwd,
                       const std::vector<std::string>& extra_env,
                       std::string& error) {
    if (argv.empty()) {
        error = "no command given";
        return false;
    }

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }

    if (pid == 0) {
        // --- child ---------------------------------------------------------
        // Nothing here may allocate or throw in a way that matters: on any
        // failure the child calls _exit directly rather than unwinding through
        // a parent's destructors.
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);

        // Its own process group, so terminate() can signal the whole build
        // tree -- cmake spawns make, which spawns compilers, and killing only
        // cmake would leave those running.
        ::setpgid(0, 0);

        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            ::_exit(127);
        }
        for (const std::string& entry : extra_env) {
            ::putenv(const_cast<char*>(entry.c_str()));
        }

        std::vector<char*> raw = to_argv(argv);
        ::execvp(raw[0], raw.data());
        ::_exit(127);  // only reached when exec failed
    }

    // --- parent ------------------------------------------------------------
    ::close(fds[1]);
    pid_    = pid;
    fd_     = fds[0];
    status_ = -1;
    reaped_ = false;
    eof_    = false;
    buffer_.clear();
    return true;
}

bool Subprocess::read_line(std::string& line) {
    for (;;) {
        // Serve a complete line out of what we already have before reading
        // more, so a chunk containing several lines is not lost.
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }

        if (eof_) {
            // Whatever is left is a final line with no terminator.
            if (buffer_.empty()) {
                return false;
            }
            line = std::move(buffer_);
            buffer_.clear();
            return true;
        }

        char chunk[4096];
        const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
        if (got > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(got));
        } else if (got == 0) {
            eof_ = true;
        } else if (errno != EINTR) {
            eof_ = true;
        }
    }
}

int Subprocess::wait() {
    if (reaped_) {
        return status_;
    }
    if (pid_ <= 0) {
        return -1;
    }

    int raw = 0;
    while (::waitpid(pid_, &raw, 0) < 0) {
        if (errno != EINTR) {
            reaped_ = true;
            status_ = -1;
            pid_    = -1;
            return status_;
        }
    }

    if (WIFEXITED(raw)) {
        status_ = WEXITSTATUS(raw);
    } else if (WIFSIGNALED(raw)) {
        status_ = 128 + WTERMSIG(raw);
    } else {
        status_ = -1;
    }
    reaped_ = true;
    pid_    = -1;
    return status_;
}

void Subprocess::terminate() {
    if (pid_ <= 0) {
        return;
    }
    // Negative pid signals the whole process group, which is why the child put
    // itself in one: a build is cmake -> make -> a dozen compilers.
    ::kill(-pid_, SIGTERM);
    close_pipe();  // unblocks a read_line() waiting on output
}

bool on_path(const std::string& program) {
    if (program.empty()) {
        return true;
    }
    if (program.find('/') != std::string::npos) {
        return ::access(program.c_str(), X_OK) == 0;
    }

    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return false;
    }

    const std::string haystack(path);
    std::size_t start = 0;
    while (start <= haystack.size()) {
        const std::size_t end = haystack.find(':', start);
        const std::string dir =
            haystack.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && ::access((dir + "/" + program).c_str(), X_OK) == 0) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

}  // namespace crucible::util
