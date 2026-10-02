#include "namchain/daemon/config.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <json.hpp>

namespace namchain
{
namespace daemon
{
namespace
{

bool regular_file(const std::filesystem::path& path)
{
  std::error_code ignored;
  return std::filesystem::is_regular_file(path, ignored);
}

bool parse_unsigned(const std::string& name, const std::string& text, unsigned& out, std::string& error)
{
  if (text.empty())
  {
    error = "--" + name + " needs a value";
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' || text.front() == '-')
  {
    error = "--" + name + ": '" + text + "' is not a whole number";
    return false;
  }
  if (value > 0xfffffffful)
  {
    error = "--" + name + ": '" + text + "' is out of range";
    return false;
  }
  out = static_cast<unsigned>(value);
  return true;
}

bool parse_int(const std::string& name, const std::string& text, int& out, std::string& error)
{
  if (text.empty())
  {
    error = "--" + name + " needs a value";
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' || value < -2147483647l || value > 2147483647l)
  {
    error = "--" + name + ": '" + text + "' is not a whole number";
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

bool parse_double(const std::string& name, const std::string& text, double& out, std::string& error)
{
  if (text.empty())
  {
    error = "--" + name + " needs a value";
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (errno != 0 || end == text.c_str() || *end != '\0')
  {
    error = "--" + name + ": '" + text + "' is not a number";
    return false;
  }
  out = value;
  return true;
}
/// Socket permission bits are written in the config as an octal string ("0660") because JSON has no
/// octal literal, and as a flag in the same spelling. Parsed explicitly rather than with strtoul's
/// base 8, so "8" is an error instead of a silent zero.
bool parse_octal_mode(const std::string& name, const std::string& text, unsigned& out, std::string& error)
{
  if (text.empty())
  {
    error = "--" + name + " needs a value";
    return false;
  }
  unsigned value = 0;
  for (const char c : text)
  {
    if (c < '0' || c > '7')
    {
      error = "--" + name + ": '" + text + "' is not an octal mode";
      return false;
    }
    value = (value << 3) | static_cast<unsigned>(c - '0');
  }
  if (value > 0777u)
  {
    error = "--" + name + ": '" + text + "' is wider than 0777";
    return false;
  }
  out = value;
  return true;
}

bool read_unsigned(const nlohmann::json& doc, const char* key, unsigned& out, const std::string& path,
                   std::string& error)
{
  if (!doc.contains(key))
    return true;
  const auto& value = doc[key];
  if (!value.is_number_integer() && !value.is_number_unsigned())
  {
    error = path + ": '" + key + "' must be a whole number";
    return false;
  }
  const long long number = value.get<long long>();
  if (number < 0 || number > 0xffffffffll)
  {
    error = path + ": '" + key + "' is out of range";
    return false;
  }
  out = static_cast<unsigned>(number);
  return true;
}

bool read_int(const nlohmann::json& doc, const char* key, int& out, const std::string& path, std::string& error)
{
  if (!doc.contains(key))
    return true;
  const auto& value = doc[key];
  if (!value.is_number_integer())
  {
    error = path + ": '" + key + "' must be a whole number";
    return false;
  }
  const long long number = value.get<long long>();
  if (number < -2147483647ll || number > 2147483647ll)
  {
    error = path + ": '" + key + "' is out of range";
    return false;
  }
  out = static_cast<int>(number);
  return true;
}

bool read_bool(const nlohmann::json& doc, const char* key, bool& out, const std::string& path, std::string& error)
{
  if (!doc.contains(key))
    return true;
  if (!doc[key].is_boolean())
  {
    error = path + ": '" + key + "' must be a boolean";
    return false;
  }
  out = doc[key].get<bool>();
  return true;
}

bool read_double(const nlohmann::json& doc, const char* key, double& out, const std::string& path,
                 std::string& error)
{
  if (!doc.contains(key))
    return true;
  if (!doc[key].is_number())
  {
    error = path + ": '" + key + "' must be a number";
    return false;
  }
  out = doc[key].get<double>();
  return true;
}

bool read_string(const nlohmann::json& doc, const char* key, std::string& out, const std::string& path,
                 std::string& error)
{
  if (!doc.contains(key))
    return true;
  if (!doc[key].is_string())
  {
    error = path + ": '" + key + "' must be a string";
    return false;
  }
  out = doc[key].get<std::string>();
  return true;
}

} // namespace

bool load_config_file(const std::string& path, Config& config, std::string& error)
{
  std::ifstream file(path);
  if (!file)
  {
    error = "cannot open config '" + path + "'";
    return false;
  }
  std::ostringstream contents;
  contents << file.rdbuf();

  nlohmann::json doc;
  try
  {
    doc = nlohmann::json::parse(contents.str());
  }
  catch (const std::exception& e)
  {
    error = path + ": not valid JSON: " + e.what();
    return false;
  }
  if (!doc.is_object())
  {
    error = path + ": the config must be a JSON object";
    return false;
  }

  // Every accepted key, so an unknown one can be named rather than ignored.
  static const char* const kKeys[] = {"backend",    "device",       "sample_rate",     "period_size",
                                      "periods",    "capture_channels", "playback_channels", "rig",
                                      "rigs_root",  "socket",      "socket_mode",     "rt_priority",
                                      "cpu",        "lock_memory",  "output_queue_periods",
                                      "output_queue_floor_periods", "fade_ms", "model_budget_kb", "device_retry_ms",
                                      "render_threads",
                                      "reclamation_timeout_ms", "verbose", "capture_path", "playback_path",
                                      "tcp_listen", "tcp_address", "tcp_port", "tcp_max_connections",
                                      "tcp_max_message_bytes", "tcp_token", "tcp_token_file",
                                      "tcp_auth_timeout_ms", "models_root", "irs_root", "record_dir",
                                      "capture_device_path", "input_device", "device_buffer_frames"};
  for (const auto& [key, value] : doc.items())
  {
    bool known = false;
    for (const char* candidate : kKeys)
      known = known || key == candidate;
    if (!known)
    {
      error = path + ": unknown config key '" + key + "'";
      return false;
    }
    (void)value;
  }

  std::string backend;
  if (!read_string(doc, "backend", backend, path, error))
    return false;
  if (!backend.empty() && !parse_backend_kind(backend, config.backend))
  {
    error = path + ": unknown backend '" + backend + "'";
    return false;
  }

  if (!read_string(doc, "device", config.device, path, error) ||
      !read_string(doc, "capture_path", config.capture_path, path, error) ||
      !read_string(doc, "capture_device_path", config.capture_device_path, path, error) ||
      !read_string(doc, "input_device", config.input_device, path, error) ||
      !read_string(doc, "playback_path", config.playback_path, path, error) ||
      !read_string(doc, "rig", config.rig, path, error) ||
      !read_string(doc, "rigs_root", config.rigs_root, path, error) ||
      !read_string(doc, "models_root", config.models_root, path, error) ||
      !read_string(doc, "irs_root", config.irs_root, path, error) ||
      !read_string(doc, "record_dir", config.record_dir, path, error) ||
      !read_string(doc, "socket", config.socket_path, path, error) ||
      !read_string(doc, "tcp_address", config.tcp_address, path, error) ||
      !read_string(doc, "tcp_token", config.tcp_token, path, error) ||
      !read_string(doc, "tcp_token_file", config.tcp_token_file, path, error))
    return false;

  if (doc.contains("device_buffer_frames"))
  {
    if (!doc["device_buffer_frames"].is_number_unsigned())
    {
      error = path + ": device_buffer_frames must be a non-negative integer";
      return false;
    }
    config.device_buffer_frames = doc["device_buffer_frames"].get<unsigned>();
  }

  if (doc.contains("socket_mode"))
  {
    if (!doc["socket_mode"].is_string() ||
        !parse_octal_mode("socket_mode", doc["socket_mode"].get<std::string>(), config.socket_mode, error))
    {
      if (error.empty())
        error = path + ": 'socket_mode' must be an octal string such as \"0660\"";
      else
        error = path + ": " + error;
      return false;
    }
  }

  if (!read_unsigned(doc, "sample_rate", config.sample_rate, path, error) ||
      !read_unsigned(doc, "period_size", config.period_size, path, error) ||
      !read_unsigned(doc, "periods", config.periods, path, error) ||
      !read_unsigned(doc, "capture_channels", config.capture_channels, path, error) ||
      !read_unsigned(doc, "playback_channels", config.playback_channels, path, error) ||
      !read_unsigned(doc, "output_queue_periods", config.output_queue_periods, path, error) ||
      !read_unsigned(doc, "output_queue_floor_periods", config.output_queue_floor_periods, path, error) ||
      !read_unsigned(doc, "device_retry_ms", config.device_retry_ms, path, error) ||
      !read_unsigned(doc, "model_budget_kb", config.model_budget_kb, path, error) ||
      !read_unsigned(doc, "render_threads", config.render_threads, path, error) ||
      !read_unsigned(doc, "tcp_port", config.tcp_port, path, error) ||
      !read_unsigned(doc, "tcp_max_connections", config.tcp_max_connections, path, error) ||
      !read_unsigned(doc, "tcp_max_message_bytes", config.tcp_max_message_bytes, path, error) ||
      !read_unsigned(doc, "tcp_auth_timeout_ms", config.tcp_auth_timeout_ms, path, error) ||
      !read_double(doc, "fade_ms", config.fade_ms, path, error) ||
      !read_int(doc, "rt_priority", config.rt_priority, path, error) ||
      !read_int(doc, "cpu", config.cpu, path, error) ||
      !read_int(doc, "reclamation_timeout_ms", config.reclamation_timeout_ms, path, error) ||
      !read_bool(doc, "lock_memory", config.lock_memory, path, error) ||
      !read_bool(doc, "tcp_listen", config.tcp_listen, path, error) ||
      !read_bool(doc, "verbose", config.verbose, path, error))
    return false;

  return true;
}

bool apply_option(Config& config, const std::string& name, const std::string& value, std::string& error)
{
  if (name == "verbose")
  {
    config.verbose = true;
    return true;
  }
  if (name == "lock-memory")
  {
    config.lock_memory = true;
    return true;
  }
  if (name == "no-lock-memory")
  {
    config.lock_memory = false;
    return true;
  }
  if (name == "backend")
  {
    if (!parse_backend_kind(value, config.backend))
    {
      error = "--backend: unknown backend '" + value + "'";
      return false;
    }
    return true;
  }
  if (name == "device")
  {
    config.device = value;
    return true;
  }
  if (name == "capture-path")
  {
    config.capture_path = value;
    return true;
  }
  if (name == "input-device")
  {
    config.input_device = value;
    return true;
  }
  if (name == "device-buffer-frames")
  {
    // Refused rather than silently ignored when it is not a number: this option's whole point is the
    // number, and a typo that leaves the device at 512 frames is a latency claim nobody made.
    char* end = nullptr;
    errno = 0;
    const unsigned long frames = std::strtoul(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != '\0' || errno != 0 || frames == 0 || frames > 65536)
    {
      error = "--device-buffer-frames needs a frame count between 1 and 65536, not '" + value + "'";
      return false;
    }
    config.device_buffer_frames = static_cast<unsigned>(frames);
    return true;
  }
  if (name == "capture-device-path")
  {
    config.capture_device_path = value;
    return true;
  }
  if (name == "playback-path")
  {
    config.playback_path = value;
    return true;
  }
  if (name == "rig")
  {
    config.rig = value;
    return true;
  }
  if (name == "rigs-root")
  {
    config.rigs_root = value;
    return true;
  }
  if (name == "models-root")
  {
    config.models_root = value;
    return true;
  }
  if (name == "irs-root")
  {
    config.irs_root = value;
    return true;
  }
  if (name == "socket")
  {
    if (value.empty())
    {
      error = "--socket needs a path";
      return false;
    }
    config.socket_path = value;
    return true;
  }
  if (name == "tcp-listen")
  {
    config.tcp_listen = true;
    return true;
  }
  if (name == "tcp-address")
  {
    if (value.empty())
    {
      error = "--tcp-address needs an address";
      return false;
    }
    config.tcp_address = value;
    return true;
  }
  if (name == "tcp-token")
  {
    if (value.empty())
    {
      error = "--tcp-token needs a token";
      return false;
    }
    config.tcp_token = value;
    return true;
  }
  if (name == "tcp-token-file")
  {
    if (value.empty())
    {
      error = "--tcp-token-file needs a path";
      return false;
    }
    config.tcp_token_file = value;
    return true;
  }
  if (name == "record-dir")
  {
    // Empty is allowed: it is how a command line turns recording off.
    config.record_dir = value;
    return true;
  }
  if (name == "socket-mode")
    return parse_octal_mode("socket-mode", value, config.socket_mode, error);
  if (name == "sample-rate")
    return parse_unsigned("sample-rate", value, config.sample_rate, error);
  if (name == "period-size")
    return parse_unsigned("period-size", value, config.period_size, error);
  if (name == "periods")
    return parse_unsigned("periods", value, config.periods, error);
  if (name == "capture-channels")
    return parse_unsigned("capture-channels", value, config.capture_channels, error);
  if (name == "playback-channels")
    return parse_unsigned("playback-channels", value, config.playback_channels, error);
  if (name == "output-queue-periods")
    return parse_unsigned("output-queue-periods", value, config.output_queue_periods, error);
  if (name == "output-queue-floor-periods")
    return parse_unsigned("output-queue-floor-periods", value, config.output_queue_floor_periods, error);
  if (name == "device-retry-ms")
    return parse_unsigned("device-retry-ms", value, config.device_retry_ms, error);
  if (name == "model-budget-kb")
    return parse_unsigned("model-budget-kb", value, config.model_budget_kb, error);
  if (name == "tcp-port")
    return parse_unsigned("tcp-port", value, config.tcp_port, error);
  if (name == "tcp-max-connections")
    return parse_unsigned("tcp-max-connections", value, config.tcp_max_connections, error);
  if (name == "tcp-max-message-bytes")
    return parse_unsigned("tcp-max-message-bytes", value, config.tcp_max_message_bytes, error);
  if (name == "tcp-auth-timeout-ms")
    return parse_unsigned("tcp-auth-timeout-ms", value, config.tcp_auth_timeout_ms, error);
  if (name == "fade-ms")
    return parse_double("fade-ms", value, config.fade_ms, error);
  if (name == "reclamation-timeout-ms")
    return parse_int("reclamation-timeout-ms", value, config.reclamation_timeout_ms, error);
  if (name == "rt-priority")
    return parse_int("rt-priority", value, config.rt_priority, error);
  if (name == "cpu")
    return parse_int("cpu", value, config.cpu, error);
  if (name == "render-threads")
    return parse_unsigned("render-threads", value, config.render_threads, error);

  error = "unknown option '--" + name + "'";
  return false;
}

bool resolve_tcp_token(Config& config, std::string& error)
{
  if (!config.tcp_token.empty() && !config.tcp_token_file.empty())
  {
    error = "set either tcp_token or tcp_token_file, not both";
    return false;
  }

  if (!config.tcp_token_file.empty())
  {
    struct stat info{};
    if (::stat(config.tcp_token_file.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
    {
      error = "cannot read tcp_token_file '" + config.tcp_token_file + "'";
      return false;
    }
    std::ifstream file(config.tcp_token_file);
    if (!file)
    {
      error = "cannot read tcp_token_file '" + config.tcp_token_file + "'";
      return false;
    }
    std::string line;
    std::getline(file, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (line.empty())
    {
      // Named separately from the length check below: "the token file is empty" and "the token is too
      // short" have different fixes, and a daemon that has just read an empty file should say so.
      error = "tcp_token_file '" + config.tcp_token_file + "' is empty";
      return false;
    }
    config.tcp_token = line;

    // A warning rather than a refusal: the file may be on a filesystem whose permissions mean
    // something else, and an operator who has read this line has been told. Not gated by `verbose`,
    // because a secret every local user can read is not a debug detail.
    if ((info.st_mode & (S_IRGRP | S_IROTH)) != 0)
      std::fprintf(stderr, "[namchaind] warning: %s is mode %03o; a token other users can read is not a secret\n",
                   config.tcp_token_file.c_str(), static_cast<unsigned>(info.st_mode & 0777));
  }

  if (!config.tcp_token.empty() && config.tcp_token.size() < kMinTokenLength)
  {
    error = "the TCP token is " + std::to_string(config.tcp_token.size()) + " characters; at least " +
            std::to_string(kMinTokenLength) +
            " are required (the transport allows three attempts per connection, and connections are free)";
    return false;
  }
  if (config.tcp_listen && config.tcp_token.empty())
  {
    error = "tcp_listen needs a token: set tcp_token (visible in `ps`; for a run) or tcp_token_file "
            "(for an appliance)";
    return false;
  }
  return true;
}

bool resolve_rig_name(const Config& config, const std::string& name, std::string& path, std::string& error)
{
  if (name.empty())
  {
    error = "rig.load needs a 'rig' name";
    return false;
  }
  if (std::filesystem::path(name).is_absolute())
  {
    error = "rig name '" + name + "' must be relative to the rigs root";
    return false;
  }

  std::error_code ec;
  const std::filesystem::path root = std::filesystem::weakly_canonical(config.rigs_root, ec);
  if (ec)
  {
    error = "rigs root '" + config.rigs_root + "' is not usable: " + ec.message();
    return false;
  }
  const std::filesystem::path full = std::filesystem::weakly_canonical(root / name, ec);
  if (ec)
  {
    error = "cannot resolve rig '" + name + "': " + ec.message();
    return false;
  }

  // The canonical paths are compared as a prefix, so a symlink inside the root that points outside
  // is refused here rather than read. `weakly_canonical` has already collapsed `..`, so a name that
  // walks up and out fails the same check. A root of "/" has no separator to demand.
  const std::string root_text = root.string();
  const std::string full_text = full.string();
  const bool inside = full_text.size() > root_text.size() &&
                      full_text.compare(0, root_text.size(), root_text) == 0 &&
                      (root_text.empty() || root_text.back() == '/' || full_text[root_text.size()] == '/');
  if (!inside)
  {
    error = "rig '" + name + "' is outside the rigs root";
    return false;
  }
  if (!regular_file(full))
  {
    error = "no such rig '" + name + "' under " + config.rigs_root;
    return false;
  }
  path = full_text;
  return true;
}

bool resolve_startup_rig(const Config& config, const std::string& spec, std::string& path, std::string& error)
{
  if (spec.empty())
  {
    error = "no rig was named";
    return false;
  }
  const std::filesystem::path candidate(spec);
  if (candidate.is_absolute() || regular_file(candidate))
  {
    if (!regular_file(candidate))
    {
      error = "no such rig '" + spec + "'";
      return false;
    }
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(candidate, ec);
    if (ec)
    {
      error = "cannot canonicalise '" + spec + "': " + ec.message();
      return false;
    }
    path = canonical.string();
    return true;
  }
  // Not a path from the working directory, so read it as a name under the rigs root.
  return resolve_rig_name(config, spec, path, error);
}

} // namespace daemon
} // namespace namchain
