#pragma once

// The four things the control transport needs that POSIX does not agree on.
//
// The daemon was written against Linux and says so in one place: signalfd, accept4, the
// SOCK_CLOEXEC/SOCK_NONBLOCK socket flags, and a Unix SOCK_SEQPACKET listener. None of the
// four is load-bearing -- the daemon wants "a readable fd that reports signals", "an
// accepted fd that is close-on-exec and non-blocking", and "one message in, one message
// out". Those are available everywhere; only their spelling is Linux's.
//
// This header is that spelling, so that src/daemon/daemon.cpp stays one implementation
// rather than two interleaved by preprocessor. It had exactly one `#if` in 2856 lines
// before this existed and it still does.
#include <csignal>
#include <string>

namespace namchain
{
namespace daemon
{
namespace platform
{

/// \brief Whether the local (Unix-socket) transport is a byte stream rather than datagrams.
///
/// `PF_LOCAL` has no `SOCK_SEQPACKET` on macOS -- `socket()` returns `EPROTONOSUPPORT` --
/// so there the local listener is a `SOCK_STREAM` and its peers are read with the length
/// prefix the TCP transport already uses. That costs nothing: framing.h's whole point is
/// that "the framed form is byte-identical to what the Unix `SOCK_SEQPACKET` transport
/// carries", so the two transports were already carrying the same bytes with different
/// envelopes. A stream local socket just uses the envelope that survives everywhere.
///
/// A `constexpr bool` rather than an `#ifdef` at each use, so the three places that ask
/// read as ordinary code and the compiler still folds them away.
#if defined(__linux__)
inline constexpr bool local_transport_is_stream = false;
#else
inline constexpr bool local_transport_is_stream = true;
#endif

/// The socket type for the local listener: `SOCK_SEQPACKET` or `SOCK_STREAM`.
int local_socket_type();

/// \brief `socket()` with the descriptor close-on-exec and non-blocking.
///
/// Linux takes both as flags in \p type; everywhere else it is two `fcntl` calls after the
/// fact. The race that `SOCK_CLOEXEC` closes -- a `fork` between the two -- is not one this
/// daemon runs: it never forks, and the flag is set before the fd is given to anything.
/// \return the descriptor, or -1 with `errno` set.
int open_socket(int domain, int type, int protocol);

/// \brief `accept()` with the accepted descriptor close-on-exec and non-blocking.
///
/// The Linux spelling is `accept4`. \return the descriptor, or -1 with `errno` set.
int accept_socket(int listener);

/// \brief A readable descriptor that reports the signals in \p mask, which the caller has
/// already blocked in every thread.
///
/// Linux has `signalfd`. Elsewhere this starts one thread that does nothing but `sigwait`
/// and write the signal number into a pipe, which keeps the property the blocking was for:
/// the signals stay blocked everywhere, exactly one reader consumes them, and no handler
/// runs on a thread that was in the middle of something. The read end is non-blocking, so
/// the caller's drain loop is the same loop either way.
///
/// \return the read end, or -1 with \p error set.
int open_signal_reader(const sigset_t& mask, std::string& error);

/// \brief Take one signal from \p fd. \return false when there is nothing waiting.
///
/// Hides the difference between a `signalfd_siginfo` and a byte, which is all the caller
/// ever wanted: `SIGHUP` reloads, the other two stop.
bool read_signal(int fd, int& signo);

/// Close a descriptor from \ref open_signal_reader and stop the thread behind it, if any.
void close_signal_reader(int fd);

} // namespace platform
} // namespace daemon
} // namespace namchain
