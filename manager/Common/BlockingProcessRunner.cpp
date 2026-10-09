#include "Common/BlockingProcessRunner.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace managerkit {

BlockingProcessCancellation::BlockingProcessCancellation()
    : _cancelled(false), _process_group(0) {}

void BlockingProcessCancellation::setProcessGroup(int process_group) {
    std::lock_guard<std::mutex> lock(_mutex);
    _process_group = process_group;
    if (_cancelled.load() && _process_group > 0) {
#if !defined(_WIN32)
        ::kill(-_process_group, SIGTERM);
#endif
    }
}

void BlockingProcessCancellation::cancel() {
    _cancelled.store(true);
    std::lock_guard<std::mutex> lock(_mutex);
#if !defined(_WIN32)
    if (_process_group > 0)
        ::kill(-_process_group, SIGTERM);
#endif
}

bool BlockingProcessCancellation::cancelled() const {
    return _cancelled.load();
}

namespace {

#if !defined(_WIN32)

void appendBounded(std::string &target, const char *data, size_t size,
                   size_t limit, bool &truncated) {
    if (target.size() >= limit) {
        truncated = truncated || size > 0;
        return;
    }
    const size_t available = limit - target.size();
    const size_t accepted = std::min(size, available);
    target.append(data, accepted);
    truncated = truncated || accepted < size;
}

// Runs in the forked child, so only async-signal-safe calls. Without it ffmpeg inherits every
// socket of the server, including listening ones, and keeps the ports bound after a restart.
void closeInheritedFds() {
#if defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3U, ~0U, 0U) == 0)
        return;
#endif
    struct rlimit limit;
    int max_fd = 1024;
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
        max_fd = static_cast<int>(limit.rlim_cur);
    for (int fd = 3; fd < max_fd; ++fd)
        ::close(fd);
}

void drainFd(int fd, std::string &target, size_t limit, bool &truncated) {
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            appendBounded(target, buffer, static_cast<size_t>(count), limit, truncated);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
}

#endif

} // namespace

BlockingProcessResult BlockingProcessRunner::run(const BlockingProcessRequest &request) {
    BlockingProcessResult result;
    if (request.executable.empty()) {
        result.error = "Process executable is empty";
        return result;
    }

#if defined(_WIN32)
    result.error = "Tier transcoding process execution is unavailable on Windows";
    return result;
#else

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    // O_CLOEXEC: a child forked concurrently by another thread must not hold our write ends.
    if (::pipe2(stdout_pipe, O_CLOEXEC) != 0 || ::pipe2(stderr_pipe, O_CLOEXEC) != 0) {
        result.error = std::string("Cannot create process pipe: ") + std::strerror(errno);
        if (stdout_pipe[0] >= 0) { ::close(stdout_pipe[0]); ::close(stdout_pipe[1]); }
        if (stderr_pipe[0] >= 0) { ::close(stderr_pipe[0]); ::close(stderr_pipe[1]); }
        return result;
    }

    // Built before fork(): the child must not allocate.
    std::vector<char *> args;
    args.reserve(request.argv.size() + 2);
    args.push_back(const_cast<char *>(request.executable.c_str()));
    for (size_t i = 0; i < request.argv.size(); ++i)
        args.push_back(const_cast<char *>(request.argv[i].c_str()));
    args.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        result.error = std::string("Cannot fork process: ") + std::strerror(errno);
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        return result;
    }

    if (child == 0) {
        ::setpgid(0, 0);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        closeInheritedFds();
        ::execvp(args[0], args.data());
        static const char kExecFailed[] = "exec failed\n";
        ssize_t ignored = ::write(STDERR_FILENO, kExecFailed, sizeof(kExecFailed) - 1);
        (void)ignored;
        ::_exit(127);
    }

    ::setpgid(child, child);
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);
    if (request.cancellation)
        request.cancellation->setProcessGroup(static_cast<int>(child));

    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    int status = 0;
    bool terminated = false;
    std::chrono::steady_clock::time_point terminated_at;
    for (;;) {
        drainFd(stdout_pipe[0], result.standard_output, request.output_limit_bytes,
                result.output_truncated);
        drainFd(stderr_pipe[0], result.standard_error, request.output_limit_bytes,
                result.output_truncated);

        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child)
            break;
        if (waited < 0 && errno != EINTR) {
            result.error = std::string("Cannot wait for process: ") + std::strerror(errno);
            break;
        }

        const bool cancelled = request.cancellation && request.cancellation->cancelled();
        const bool timed_out = request.timeout_seconds > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started).count() >= request.timeout_seconds;
        if ((cancelled || timed_out) && !terminated) {
            result.cancelled = cancelled;
            result.timed_out = timed_out;
            ::kill(-child, SIGTERM);
            terminated = true;
            terminated_at = std::chrono::steady_clock::now();
        } else if (terminated &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - terminated_at).count() >= 1000LL) {
            ::kill(-child, SIGKILL);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    drainFd(stdout_pipe[0], result.standard_output, request.output_limit_bytes,
            result.output_truncated);
    drainFd(stderr_pipe[0], result.standard_error, request.output_limit_bytes,
            result.output_truncated);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);
    if (request.cancellation)
        request.cancellation->setProcessGroup(0);

    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);
    return result;
#endif
}

} // namespace managerkit
