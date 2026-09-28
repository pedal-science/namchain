#pragma once

// The TCP control transport's framing (docs/ui-integration-plan.md §1.8).
//
// A stream socket has no message boundaries, so the framing is the smallest thing that puts them
// back: a u32 little-endian byte count followed by exactly that many bytes of one JSON body. The body
// is byte-identical to what the Unix `SOCK_SEQPACKET` transport carries, which is what keeps one
// request handler, one set of responses and one protocol document for both transports.
//
// Shared by the daemon, `namchainctl` and the transport tests rather than written three times: the
// place a hand-rolled client goes wrong is exactly here, and a length that means two different things
// to two sides is a bug that only shows up under load.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace namchain
{
namespace daemon
{

/// Bytes of length prefix in front of every body on the TCP transport.
inline constexpr std::size_t kFrameHeaderBytes = 4;

/// \brief The largest reply: the most undelivered reply a connection may hold before the daemon gives
/// up on it, and so what a client must be ready to read.
///
/// Bounded apart from requests (`Config::tcp_max_message_bytes`), because the two grow with different
/// things. A request is what a client chose to send. A reply lists what the daemon holds, and
/// `engine.assets` and `engine.schema` carry the whole library: about 1 MB each for 1,500 impulse
/// responses, so 16 MiB is room for about 20,000.
inline constexpr std::size_t kMaxReplyBytes = 16u << 20;

/// \brief Append one framed message to \p out.
///
/// The length is written little-endian by hand rather than by copying a `uint32_t`, because the wire
/// format is a byte order and not whatever this machine happens to use.
inline void append_frame(std::vector<char>& out, const char* body, std::size_t size)
{
  const std::uint32_t length = static_cast<std::uint32_t>(size);
  const char header[kFrameHeaderBytes] = {
    static_cast<char>(length & 0xffu), static_cast<char>((length >> 8) & 0xffu),
    static_cast<char>((length >> 16) & 0xffu), static_cast<char>((length >> 24) & 0xffu)};
  out.insert(out.end(), header, header + kFrameHeaderBytes);
  out.insert(out.end(), body, body + size);
}

/// \brief Append one framed message to \p out.
inline void append_frame(std::vector<char>& out, const std::string& body)
{
  append_frame(out, body.data(), body.size());
}

/// \brief The length a frame header declares. \p header must hold \ref kFrameHeaderBytes bytes.
inline std::uint32_t frame_length(const char* header)
{
  const auto* bytes = reinterpret_cast<const unsigned char*>(header);
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

} // namespace daemon
} // namespace namchain
