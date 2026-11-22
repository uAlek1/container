#include "terminal.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "runtime.pb.h"

namespace {

bool write_all(int fd, const char* buf, ssize_t n) {
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += w;
    }
    return true;
}

volatile sig_atomic_t g_winch = 0;
extern "C" void winch_handler(int) { g_winch = 1; }

bool enter_raw(int fd, struct termios* saved) {
    if (!::isatty(fd)) return false;
    if (::tcgetattr(fd, saved) < 0) return false;
    struct termios raw = *saved;
    ::cfmakeraw(&raw);
    // cfmakeraw сбрасывает OPOST — вернём, иначе вывод "лестницей".
    raw.c_oflag |= OPOST;
    return ::tcsetattr(fd, TCSANOW, &raw) == 0;
}

void restore(int fd, const struct termios* saved) {
    if (saved) ::tcsetattr(fd, TCSANOW, saved);
}

void send_resize(cli::Client& client, int tty_fd) {
    struct winsize ws{};
    if (::ioctl(tty_fd, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) {
        return;
    }
    runtime::v1::ClientMessage msg;
    auto* r = msg.mutable_attach()->mutable_resize();
    r->set_rows(ws.ws_row);
    r->set_cols(ws.ws_col);
    try {
        client.send(msg);
    } catch (const std::exception&) {
        // Сокет умер — основной цикл это увидит.
    }
}

}  // namespace

namespace terminal {

RawMode::RawMode(int fd) : fd_(fd) {
    if (!enter_raw(fd_, &saved_)) {
        throw std::runtime_error("attach requires a TTY on stdin");
    }
}

RawMode::~RawMode() {
    restore(fd_, &saved_);
}

int attach_loop(cli::Client& client, int host_in, int host_out) {
    try {
        runtime::v1::ClientMessage req;
        req.mutable_attach();
        client.send(req);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "attach: %s\n", e.what());
        return 1;
    }

    send_resize(client, host_in);

    struct sigaction old_sa{};
    struct sigaction sa{};
    sa.sa_handler = winch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    bool winch_installed = false;
    if (::sigaction(SIGWINCH, &sa, &old_sa) == 0) {
        winch_installed = true;
    }

    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        if (winch_installed) ::sigaction(SIGWINCH, &old_sa, nullptr);
        std::fprintf(stderr, "attach: epoll_create1: %s\n", std::strerror(errno));
        return 1;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = host_in;
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, host_in, &ev);

    ev.events = EPOLLIN;
    ev.data.fd = client.fd();
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, client.fd(), &ev);

    int exit_code = 1;
    bool stdin_open = true;
    char buf[4096];
    struct epoll_event events[2];

    for (;;) {
        if (g_winch) {
            g_winch = 0;
            send_resize(client, host_in);
        }

        int nfds = ::epoll_wait(epfd, events, 2, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "attach: epoll_wait: %s\n", std::strerror(errno));
            break;
        }

        bool done = false;
        for (int i = 0; i < nfds && !done; ++i) {
            int fd = events[i].data.fd;

            if (fd == host_in && stdin_open) {
                ssize_t n = ::read(host_in, buf, sizeof(buf));
                if (n > 0) {
                    runtime::v1::ClientMessage msg;
                    msg.mutable_attach()->set_data(std::string(buf, static_cast<size_t>(n)));
                    try {
                        client.send(msg);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "attach: %s\n", e.what());
                        done = true;
                    }
                } else if (n == 0) {
                    stdin_open = false;
                    ::epoll_ctl(epfd, EPOLL_CTL_DEL, host_in, nullptr);
                    runtime::v1::ClientMessage msg;
                    msg.mutable_attach()->set_eof(true);
                    try {
                        client.send(msg);
                    } catch (const std::exception&) {
                        done = true;
                    }
                } else if (errno != EINTR) {
                    done = true;
                }
            } else if (fd == client.fd()) {
                runtime::v1::ServerMessage resp;
                try {
                    if (!client.recv(resp, false)) {
                        done = true;
                        break;
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "attach: %s\n", e.what());
                    done = true;
                    break;
                }

                if (!resp.has_attach()) {
                    std::fprintf(stderr, "attach: unexpected response type\n");
                    done = true;
                    break;
                }

                const auto& out = resp.attach();
                if (out.has_data()) {
                    const std::string& data = out.data();
                    if (!write_all(host_out, data.data(), static_cast<ssize_t>(data.size()))) {
                        done = true;
                    }
                } else if (out.has_exit()) {
                    const auto& ex = out.exit();
                    exit_code = ex.signal() != 0 ? 128 + ex.signal() : ex.exit_code();
                    done = true;
                }
            }
        }
        if (done) break;
    }

    ::close(epfd);
    if (winch_installed) ::sigaction(SIGWINCH, &old_sa, nullptr);
    return exit_code;
}

}  // namespace terminal