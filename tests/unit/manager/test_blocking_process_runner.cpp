#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "Common/BlockingProcessRunner.h"

using namespace managerkit;

TEST(BlockingProcessRunnerTest, PassesArgumentsWithoutShellInterpretation) {
    BlockingProcessRequest request;
    request.executable = "/usr/bin/printf";
    request.argv = {"%s", "literal;$(touch /tmp/should-not-exist)"};
    BlockingProcessResult result = BlockingProcessRunner().run(request);
    ASSERT_TRUE(result.success()) << result.standard_error;
    EXPECT_EQ("literal;$(touch /tmp/should-not-exist)", result.standard_output);
}

TEST(BlockingProcessRunnerTest, ReportsNonZeroAndBoundsOutput) {
    BlockingProcessRequest request;
    request.executable = "/usr/bin/printf";
    request.argv = {"123456789"};
    request.output_limit_bytes = 4;
    BlockingProcessResult result = BlockingProcessRunner().run(request);
    EXPECT_TRUE(result.success());
    EXPECT_EQ("1234", result.standard_output);
    EXPECT_TRUE(result.output_truncated);
}

TEST(BlockingProcessRunnerTest, TimesOutAndReapsChild) {
    BlockingProcessRequest request;
    request.executable = "/usr/bin/sleep";
    request.argv = {"10"};
    request.timeout_seconds = 1;
    const auto started = std::chrono::steady_clock::now();
    BlockingProcessResult result = BlockingProcessRunner().run(request);
    EXPECT_TRUE(result.timed_out);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - started).count(), 4);
}

// ffmpeg used to inherit every server socket, so a restart could not rebind the ports.
TEST(BlockingProcessRunnerTest, ChildDoesNotInheritServerFds) {
    const int fd = ::open("/dev/null", O_RDONLY);  // no O_CLOEXEC, like an accepted socket
    ASSERT_GE(fd, 3);
    BlockingProcessRequest request;
    request.executable = "/bin/sh";
    request.argv = {"-c", "test -e /proc/self/fd/" + std::to_string(fd)};
    BlockingProcessResult result = BlockingProcessRunner().run(request);
    ::close(fd);
    EXPECT_EQ(1, result.exit_code) << result.standard_error;
}
