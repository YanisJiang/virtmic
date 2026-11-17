#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace virtmic {

inline constexpr uint32_t kProtocolMagic = 0x564D4943;  // 'VMIC'

enum class Command : uint32_t {
  Start = 1,
  Stop = 2,
  SetBypass = 3,
  SimulateInferTimeout = 4,
};

enum class EventType : uint32_t {
  Started = 1,
  Stopped = 2,
  InferTimeout = 3,
  Error = 4,
  Latency = 5,
};

struct MessageHeader {
  uint32_t magic;
  uint32_t type;
  uint32_t nbytes;
};

struct BypassPayload {
  uint32_t enabled;
};

struct EmptyPayload {};

struct DecodedCommand {
  Command cmd;
  BypassPayload bypass{};
};

struct DecodedEvent {
  EventType type;
};

namespace detail {

inline void append_u32(std::vector<uint8_t>& out, uint32_t v) {
  const std::size_t off = out.size();
  out.resize(off + sizeof(uint32_t));
  std::memcpy(out.data() + off, &v, sizeof(uint32_t));
}

inline bool read_u32(const uint8_t* data, std::size_t nbytes, std::size_t& off,
                     uint32_t& out) {
  if (off + sizeof(uint32_t) > nbytes) return false;
  std::memcpy(&out, data + off, sizeof(uint32_t));
  off += sizeof(uint32_t);
  return true;
}

inline std::vector<uint8_t> encode_header(uint32_t type, uint32_t nbytes) {
  std::vector<uint8_t> out;
  out.reserve(sizeof(MessageHeader) + nbytes);
  append_u32(out, kProtocolMagic);
  append_u32(out, type);
  append_u32(out, nbytes);
  return out;
}

inline bool parse_header(const std::vector<uint8_t>& bytes, MessageHeader& hdr,
                         std::size_t& off) {
  off = 0;
  if (!read_u32(bytes.data(), bytes.size(), off, hdr.magic)) return false;
  if (hdr.magic != kProtocolMagic) return false;
  if (!read_u32(bytes.data(), bytes.size(), off, hdr.type)) return false;
  if (!read_u32(bytes.data(), bytes.size(), off, hdr.nbytes)) return false;
  if (off + hdr.nbytes > bytes.size()) return false;
  return true;
}

}  // namespace detail

inline std::vector<uint8_t> encode_command(Command cmd) {
  return detail::encode_header(static_cast<uint32_t>(cmd), 0);
}

inline std::vector<uint8_t> encode_command(Command cmd, BypassPayload payload) {
  auto out = detail::encode_header(static_cast<uint32_t>(cmd),
                                   sizeof(uint32_t));
  detail::append_u32(out, payload.enabled);
  return out;
}

inline std::optional<DecodedCommand> decode_command(
    const std::vector<uint8_t>& bytes) {
  MessageHeader hdr{};
  std::size_t off = 0;
  if (!detail::parse_header(bytes, hdr, off)) return std::nullopt;

  const auto cmd = static_cast<Command>(hdr.type);
  DecodedCommand decoded{cmd, {}};

  if (cmd == Command::SetBypass) {
    if (hdr.nbytes < sizeof(uint32_t)) return std::nullopt;
    uint32_t enabled = 0;
    if (!detail::read_u32(bytes.data(), bytes.size(), off, enabled)) {
      return std::nullopt;
    }
    decoded.bypass.enabled = enabled;
  } else if (hdr.nbytes != 0) {
    // Start / Stop / SimulateInferTimeout expect no payload.
    return std::nullopt;
  }

  return decoded;
}

inline std::vector<uint8_t> encode_event(EventType type,
                                         EmptyPayload = {}) {
  return detail::encode_header(static_cast<uint32_t>(type), 0);
}

inline std::optional<DecodedEvent> decode_event(
    const std::vector<uint8_t>& bytes) {
  MessageHeader hdr{};
  std::size_t off = 0;
  if (!detail::parse_header(bytes, hdr, off)) return std::nullopt;
  return DecodedEvent{static_cast<EventType>(hdr.type)};
}

}  // namespace virtmic
