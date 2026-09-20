// The platform spellings behind namchain/daemon/platform.h. See that header for why.
#include "namchain/daemon/platform.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#if defined(__linux__)
  #include <sys/signalfd.h>
#endif

namespace namchain
{
namespace daemon
{
namespace platform
{
#if !defined(__linux__)
namespace
{

/// Set both flags on an existing descriptor. The Linux paths never call this; everywhere
/// else it is what `SOCK_CLOEXEC | SOCK_NONBLOCK` means. \return false with `errno` set.
bool make_cloexec_nonblocking(int fd)
{
  const int descriptor_flags = ::fcntl(fd, F_GETFD);
  if (descriptor_flags < 0 || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
    return false;
  const int status_flags = ::fcntl(fd, F_GETFL);
  if (status_flags < 0 || ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0)
    return false;
  return true;
}

} // namespace
#endif // not Linux

int local_socket_type()
{
  return local_transport_is_stream ? SOCK_STREAM : SOCK_SEQPACKET;
}

#if defined(__linux__)

int open_socket(int domain, int type, int protocol)
{
  return ::socket(domain, type | SOCK_CLOEXEC | SOCK_NONBLOCK, protocol);
}

int accept_socket(int listener)
{
  return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
}

int open_signal_reader(const sigset_t& mask, std::string& error)
{
  const int fd = ::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (fd < 0)
    error = std::string("signalfd: ") + std::strerror(errno);
  return fd;
}

bool read_signal(int fd, int& signo)
{
  signalfd_siginfo info{};
  if (::read(fd, &info, sizeof(info)) != static_cast<ssize_t>(sizeof(info)))
    return false;
  signo = static_cast<int>(info.ssi_signo);
  return true;
}

void close_signal_reader(int fd)
{
  if (fd >= 0)
    ::close(fd);
}

#else // not Linux

int open_socket(int domain, int type, int protocol)
{
  const int fd = ::socket(domain, type, protocol);
  if (fd < 0)
    return -1;
  if (!make_cloexec_nonblocking(fd))
  {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

int accept_socket(int listener)
{
  const int fd = ::accept(listener, nullptr, nullptr);
  if (fd < 0)
    return -1;
  if (!make_cloexec_nonblocking(fd))
  {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

namespace
{

/// \brief The thread standing in for signalfd, and the pipe it reports through.
///
/// One per reader, keyed by the read end, because a `Daemon` is a class and a process may
/// hold more than one over its life -- the daemon tests start and stop several. A leaked
/// thread per instance would be bad on its own; two of them racing for the same `sigwait`
/// would be worse.
struct SignalReader
{
  std::thread thread;
  std::atomic<bool> stopping{false};
  int write_fd = -1;
  pthread_t handle{};
  /// One signal from the waited set, used only to break `sigwait` at shutdown. Taken from
  /// the set rather than assumed, because a signal the thread is not waiting for would not
  /// wake it and the join would never return.
  int wake_signal = 0;
};

std::mutex readers_mutex;
std::map<int, SignalReader*>& readers()
{
  static std::map<int, SignalReader*> map;
  return map;
}

} // namespace

int open_signal_reader(const sigset_t& mask, std::string& error)
{
  int ends[2] = {-1, -1};
  if (::pipe(ends) != 0)
  {
    error = std::string("pipe: ") + std::strerror(errno);
    return -1;
  }
  // The read end matches signalfd's SFD_NONBLOCK so the caller's drain loop is the same
  // loop on both platforms. The write end stays blocking: it carries one byte at a time
  // into a pipe that is never anywhere near full, and a signal that was dropped for want
  // of buffer would be a shutdown that did not happen.
  if (!make_cloexec_nonblocking(ends[0]) || ::fcntl(ends[1], F_SETFD, FD_CLOEXEC) < 0)
  {
    error = std::string("fcntl: ") + std::strerror(errno);
    ::close(ends[0]);
    ::close(ends[1]);
    return -1;
  }

  SignalReader* reader = new SignalReader();
  reader->write_fd = ends[1];

  const sigset_t waited = mask;
  reader->thread = std::thread([reader, waited]() {
    for (;;)
    {
      int signo = 0;
      // The signals are already blocked in every thread, so this is the only consumer and
      // nothing is ever delivered to a thread that was in the middle of something. That is
      // the same property signalfd gives, arrived at the same way.
      if (::sigwait(&waited, &signo) != 0)
        continue;
      if (reader->stopping.load(std::memory_order_acquire))
        break;
      const unsigned char byte = static_cast<unsigned char>(signo);
      ssize_t written = 0;
      do
        written = ::write(reader->write_fd, &byte, 1);
      while (written < 0 && errno == EINTR);
    }
  });
  reader->handle = reader->thread.native_handle();
  // Unqualified: Darwin defines sigismember as both a function and a macro, and `::` picks a
  // fight with the macro.
  for (int candidate = 1; candidate < NSIG; ++candidate)
    if (sigismember(&waited, candidate) == 1)
    {
      reader->wake_signal = candidate;
      break;
    }

  {
    const std::lock_guard<std::mutex> lock(readers_mutex);
    readers()[ends[0]] = reader;
  }
  return ends[0];
}

bool read_signal(int fd, int& signo)
{
  unsigned char byte = 0;
  if (::read(fd, &byte, 1) != 1)
    return false;
  signo = static_cast<int>(byte);
  return true;
}

void close_signal_reader(int fd)
{
  if (fd < 0)
    return;

  SignalReader* reader = nullptr;
  {
    const std::lock_guard<std::mutex> lock(readers_mutex);
    auto found = readers().find(fd);
    if (found != readers().end())
    {
      reader = found->second;
      readers().erase(found);
    }
  }

  if (reader != nullptr)
  {
    // The flag first, then a signal *to that thread* to break it out of sigwait. The
    // signal is blocked there, so it goes pending for the thread and sigwait returns it;
    // the thread then sees the flag and leaves without reporting it. Any real signal that
    // arrived in the same instant is lost, which is the correct trade for a shutdown.
    reader->stopping.store(true, std::memory_order_release);
    if (reader->wake_signal != 0)
      ::pthread_kill(reader->handle, reader->wake_signal);
    reader->thread.join();
    ::close(reader->write_fd);
    delete reader;
  }
  ::close(fd);
}

#endif // not Linux

} // namespace platform
} // namespace daemon
} // namespace namchain
