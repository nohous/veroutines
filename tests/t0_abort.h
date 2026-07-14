/**
 * @brief Fork-based death-test support for the fatal-path tests (C6, C8).
 *
 * fatal() ends the process with std::abort(), so its tests run the scenario in
 * a forked child and inspect how it died. This header pulls in the POSIX
 * headers, whose ::signal collides with veroutines::signal under a using
 * directive - a translation unit that includes this one must spell the port
 * type veroutines::signal.
 */

#pragma once

#include "t0.h"

#include <string>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

struct abort_result {
    bool aborted;      // child died by a signal; fatal() aborts, and err confirms which
    std::string err;   // what it wrote to stderr before dying (the diagnostic)
};

// Runs fn in a forked child with its stderr captured, reporting whether the
// child died by a signal. abort() bypasses atexit, so the child runs no ASan
// leak check; a real ASan error would instead _exit(1) and read as not-aborted.
template<typename Fn>
inline abort_result run_expecting_abort(Fn fn) {
    int fds[2];
    if (pipe(fds) != 0) {
        std::perror("pipe");
        std::exit(2);
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        std::exit(2);
    }

    if (pid == 0) {
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        fn();
        _exit(0);   // returned without aborting: the parent reads this as failure
    }

    close(fds[1]);

    std::string err;
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0; ) {
        err.append(buf, static_cast<std::size_t>(n));
    }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    return {static_cast<bool>(WIFSIGNALED(status)), std::move(err)};
}
