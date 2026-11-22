#include "container_process.hpp"

#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

namespace shim {
namespace {

struct ChildArgs {
    const char* rootfs;
    char* const* argv;
    int sync_fd;
    int err_fd;
    int pty_slave_fd;
    int log_fd;
};

namespace child {

void die(int err_fd, int e) {
    int saved = e;
    (void)!::write(err_fd, &saved, sizeof(saved));
    ::_exit(127);
}

void mount_at(const char* rootfs, const char* sub, mode_t mode,
              const char* fstype, unsigned long flags, const char* data,
              int err_fd) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s%s", rootfs, sub);

    if (::mkdir(path, mode) != 0 && errno != EEXIST)
        die(err_fd, errno);

    if (::mount(fstype, path, fstype, flags, data) != 0)
        die(err_fd, errno);
}

void attach_pty(int slave_fd, int err_fd) {
    if (::setsid() < 0)
        die(err_fd, errno);
    if (::ioctl(slave_fd, TIOCSCTTY, 0) < 0)
        die(err_fd, errno);
    if (::dup2(slave_fd, STDIN_FILENO) < 0 ||
        ::dup2(slave_fd, STDOUT_FILENO) < 0 ||
        ::dup2(slave_fd, STDERR_FILENO) < 0)
        die(err_fd, errno);
    if (slave_fd > STDERR_FILENO)
        ::close(slave_fd);
}

void attach_log(int log_fd, int err_fd) {
    if (log_fd < 0)
        return;
    if (::dup2(log_fd, STDOUT_FILENO) < 0 ||
        ::dup2(log_fd, STDERR_FILENO) < 0)
        die(err_fd, errno);
    if (log_fd > STDERR_FILENO)
        ::close(log_fd);
}

int run(void* p) {
    auto* a = static_cast<ChildArgs*>(p);

    char tok = 0;
    ssize_t n = ::read(a->sync_fd, &tok, 1);
    if (n != 1 || tok != 'G')
        die(a->err_fd, n < 0 ? errno : EPIPE);

    const char* new_hostname = "shim-container";
    if (::sethostname(new_hostname, std::strlen(new_hostname)) != 0)
        die(a->err_fd, errno);

    mount_at(a->rootfs, "/proc", 0555, "proc",  0, nullptr,     a->err_fd);
    mount_at(a->rootfs, "/dev",  0755, "tmpfs", 0, "mode=0755", a->err_fd);

    // stdio: либо PTY, либо лог-файл. Взаимоисключающе.
    if (a->pty_slave_fd >= 0)
        attach_pty(a->pty_slave_fd, a->err_fd);
    else if (a->log_fd >= 0)
        attach_log(a->log_fd, a->err_fd);

    if (::chroot(a->rootfs) != 0)
        die(a->err_fd, errno);
    if (::chdir("/") != 0)
        die(a->err_fd, errno);

    ::execvp(a->argv[0], a->argv);
    die(a->err_fd, errno);
}

}  // namespace child

namespace parent {

bool write_proc(const char* path, const char* data) {
    int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t len = std::strlen(data);
    ssize_t w = ::write(fd, data, len);
    ::close(fd);
    return w == static_cast<ssize_t>(len);
}

bool map_user(pid_t pid) {
    char path[64], buf[64];
    std::snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
    std::snprintf(buf, sizeof(buf), "0 %u 1", ::getuid());
    if (!write_proc(path, buf))
        return false;

    // setgroups может отсутствовать на старых ядрах — игнорируем.
    std::snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
    if (::access(path, F_OK) == 0)
        (void)write_proc(path, "deny");

    std::snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
    std::snprintf(buf, sizeof(buf), "0 %u 1", ::getgid());
    if (!write_proc(path, buf))
        return false;

    return true;
}

}  // namespace parent

ssize_t retry_read(int fd, void* buf, size_t n) {
    for (;;) {
        ssize_t r = ::read(fd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

ssize_t retry_write(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    size_t total = 0;
    while (total < n) {
        ssize_t w = ::write(fd, p + total, n - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += static_cast<size_t>(w);
    }
    return static_cast<ssize_t>(total);
}

constexpr size_t kStack = 1 << 20;

constexpr std::uint64_t kFlags =
    CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUSER |
    CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;

void* make_stack(size_t size) {
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    return p;
}

}  // namespace

std::expected<SpawnResult, Error> spawn(const SpawnRequest& req) {
    if (req.rootfs.empty() || req.argv.empty())
        return std::unexpected(Error{0, EINVAL, "rootfs/argv empty"});
    if (!req.tty && req.log_path.empty())
        return std::unexpected(Error{0, EINVAL, "tty=false requires log_path"});

    int sync_pipe[2] = {-1, -1};
    int err_pipe[2]  = {-1, -1};
    if (::pipe2(sync_pipe, O_CLOEXEC) < 0)
        return std::unexpected(Error{1, errno, "pipe sync"});
    if (::pipe2(err_pipe, O_CLOEXEC) < 0) {
        ::close(sync_pipe[0]); ::close(sync_pipe[1]);
        return std::unexpected(Error{1, errno, "pipe err"});
    }

    int master_fd = -1;
    int slave_fd = -1;
    int log_fd = -1;
    if (req.tty) {
        master_fd = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master_fd < 0) {
            ::close(sync_pipe[0]); ::close(sync_pipe[1]);
            ::close(err_pipe[0]);  ::close(err_pipe[1]);
            return std::unexpected(Error{1, errno, "posix_openpt"});
        }
        if (::grantpt(master_fd) < 0 || ::unlockpt(master_fd) < 0) {
            int e = errno;
            ::close(master_fd);
            ::close(sync_pipe[0]); ::close(sync_pipe[1]);
            ::close(err_pipe[0]);  ::close(err_pipe[1]);
            return std::unexpected(Error{1, e, "grantpt/unlockpt"});
        }
        const char* slave_name = ::ptsname(master_fd);
        if (!slave_name) {
            int e = errno;
            ::close(master_fd);
            ::close(sync_pipe[0]); ::close(sync_pipe[1]);
            ::close(err_pipe[0]);  ::close(err_pipe[1]);
            return std::unexpected(Error{1, e, "ptsname"});
        }
        slave_fd = ::open(slave_name, O_RDWR | O_NOCTTY);
        if (slave_fd < 0) {
            int e = errno;
            ::close(master_fd);
            ::close(sync_pipe[0]); ::close(sync_pipe[1]);
            ::close(err_pipe[0]);  ::close(err_pipe[1]);
            return std::unexpected(Error{1, e, "open slave"});
        }
    } else {
        log_fd = ::open(req.log_path.c_str(),
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (log_fd < 0) {
            int e = errno;
            ::close(sync_pipe[0]); ::close(sync_pipe[1]);
            ::close(err_pipe[0]);  ::close(err_pipe[1]);
            return std::unexpected(Error{1, e, "open log"});
        }
    }

    std::vector<char*> argv;
    argv.reserve(req.argv.size() + 1);
    for (const auto& a : req.argv)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    void* stack = make_stack(kStack);
    if (!stack) {
        int e = errno;
        if (master_fd >= 0) ::close(master_fd);
        if (slave_fd  >= 0) ::close(slave_fd);
        if (log_fd    >= 0) ::close(log_fd);
        ::close(sync_pipe[0]); ::close(sync_pipe[1]);
        ::close(err_pipe[0]);  ::close(err_pipe[1]);
        return std::unexpected(Error{2, e, "mmap stack"});
    }

    ChildArgs args{
        req.rootfs.c_str(),
        argv.data(),
        sync_pipe[0],
        err_pipe[1],
        slave_fd,
        log_fd,
    };

    pid_t pid = ::clone(child::run,
                        static_cast<char*>(stack) + kStack,
                        kFlags,
                        &args);

    // Родитель: sync.r, err.w, slave (если был), log (если был) — закрыть.
    ::close(sync_pipe[0]);
    ::close(err_pipe[1]);
    if (slave_fd >= 0) ::close(slave_fd);
    if (log_fd   >= 0) ::close(log_fd);

    auto cleanup = [&]() {
        ::close(sync_pipe[1]);
        ::close(err_pipe[0]);
        if (master_fd >= 0) ::close(master_fd);
        ::munmap(stack, kStack);
    };

    if (pid < 0) {
        int e = errno;
        cleanup();
        return std::unexpected(Error{3, e, "clone"});
    }

    if (!parent::map_user(pid)) {
        int e = errno;
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        cleanup();
        return std::unexpected(Error{4, e, "map user"});
    }

    char tok = 'G';
    if (::write(sync_pipe[1], &tok, 1) != 1) {
        int e = errno;
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        cleanup();
        return std::unexpected(Error{5, e, "signal"});
    }
    ::close(sync_pipe[1]);

    int child_err = 0;
    ssize_t n = retry_read(err_pipe[0], &child_err, sizeof(child_err));
    ::close(err_pipe[0]);

    if (n < 0) {
        int e = errno;
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        if (master_fd >= 0) ::close(master_fd);
        ::munmap(stack, kStack);
        return std::unexpected(Error{6, e, "read err"});
    }
    if (n == static_cast<ssize_t>(sizeof(child_err)) && child_err != 0) {
        ::waitpid(pid, nullptr, 0);
        if (master_fd >= 0) ::close(master_fd);
        ::munmap(stack, kStack);
        return std::unexpected(Error{6, child_err, "child"});
    }

    SpawnResult result;
    result.pid = pid;
    result.master_fd = master_fd;
    result.log_fd = -1;

    ::munmap(stack, kStack);
    return result;
}

int wait_for_exit(int32_t pid) {
    int status = 0;
    pid_t r = ::waitpid(pid, &status, 0);
    if (r < 0) return -1;
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

}  // namespace shim