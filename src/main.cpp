#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <codecvt>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <thread>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef TPLINK_IPC_VERSION
#define TPLINK_IPC_VERSION "0.2.0"
#endif

using Json = nlohmann::json;

namespace {

std::string env(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value) throw std::runtime_error(std::string("missing environment variable: ") + name);
  return value;
}

struct CameraSettings {
  std::string host, user, password;
};

std::filesystem::path config_path() {
  std::filesystem::path base;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) base = xdg;
  else base = std::filesystem::path(env("HOME")) / ".config";
  if (!base.is_absolute()) throw std::runtime_error("XDG_CONFIG_HOME must be an absolute path");
  return base / "tplink-ipc-mcp" / "config.json";
}

CameraSettings load_settings() {
  CameraSettings settings;
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if ((xdg && *xdg) || std::getenv("HOME")) {
    const auto path = config_path();
    if (std::filesystem::exists(path)) {
      struct stat info{};
      if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
          (info.st_mode & 077) != 0 || info.st_size < 0 || info.st_size > 4096)
        throw std::runtime_error("camera config must be a private regular file owned by this user");
      std::ifstream in(path);
      if (!in) throw std::runtime_error("cannot read camera config");
      try {
        const Json saved = Json::parse(in);
        settings.host = saved.at("host").get<std::string>();
        settings.user = saved.at("user").get<std::string>();
        settings.password = saved.at("password").get<std::string>();
      } catch (...) { throw std::runtime_error("camera config is invalid"); }
    }
  }
  if (const char* value = std::getenv("IPC_HOST")) settings.host = value;
  if (const char* value = std::getenv("IPC_USER")) settings.user = value;
  if (const char* value = std::getenv("IPC_PASSWORD")) settings.password = value;
  return settings;
}

void save_settings(const CameraSettings& settings) {
  const auto path = config_path();
  std::filesystem::create_directories(path.parent_path());
  if (chmod(path.parent_path().c_str(), 0700) != 0) throw std::runtime_error("cannot protect camera config directory");
  std::string name = (path.parent_path() / ".config-XXXXXX").string();
  std::vector<char> temporary(name.begin(), name.end());
  temporary.push_back('\0');
  int fd = mkstemp(temporary.data());
  if (fd < 0) throw std::runtime_error("cannot create camera config");
  const std::string content = Json({{"host", settings.host}, {"user", settings.user}, {"password", settings.password}}).dump(2) + "\n";
  try {
    size_t offset = 0;
    while (offset < content.size()) {
      const ssize_t count = write(fd, content.data() + offset, content.size() - offset);
      if (count <= 0) throw std::runtime_error("cannot write camera config");
      offset += static_cast<size_t>(count);
    }
    const int sync_result = fsync(fd);
    const int close_result = close(fd);
    fd = -1;
    if (sync_result != 0 || close_result != 0) throw std::runtime_error("cannot save camera config");
    std::filesystem::rename(temporary.data(), path);
  } catch (...) {
    if (fd >= 0) close(fd);
    unlink(temporary.data());
    throw;
  }
}

std::string prompt(const std::string& label) {
  std::cerr << label << std::flush;
  std::string value;
  if (!std::getline(std::cin, value)) throw std::runtime_error("input cancelled");
  return value;
}

std::string prompt_password() {
  if (!isatty(STDIN_FILENO)) throw std::runtime_error("password entry requires an interactive terminal");
  termios original{};
  if (tcgetattr(STDIN_FILENO, &original) != 0) throw std::runtime_error("cannot read terminal settings");
  termios hidden = original;
  hidden.c_lflag &= ~ECHO;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0) throw std::runtime_error("cannot hide password input");
  try {
    const std::string password = prompt("Camera password: ");
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    std::cerr << '\n';
    return password;
  } catch (...) {
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    std::cerr << '\n';
    throw;
  }
}

std::string base64(const unsigned char* data, size_t size) {
  std::string result(4 * ((size + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), data, static_cast<int>(size));
  return result;
}

std::vector<unsigned char> unbase64(std::string data) {
  data.erase(std::remove_if(data.begin(), data.end(), [](unsigned char c) { return std::isspace(c); }), data.end());
  if (data.empty() || data.size() % 4) throw std::runtime_error("invalid base64 key");
  std::vector<unsigned char> out(data.size());
  const int length = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
  if (length < 0) throw std::runtime_error("invalid base64 key");
  out.resize(static_cast<size_t>(length) - (data.back() == '=') - (data[data.size() - 2] == '='));
  return out;
}

std::string url_decode(std::string_view input) {
  std::string result;
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int a = hex(input[i + 1]), b = hex(input[i + 2]);
      if (a >= 0 && b >= 0) { result += static_cast<char>(a * 16 + b); i += 2; continue; }
    }
    result += input[i];
  }
  return result;
}

std::string url_encode(std::string_view input) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (unsigned char c : input) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out << c;
    else out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  }
  return out.str();
}

std::string xml_escape(std::string_view input) {
  std::string out;
  for (char c : input) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\'': out += "&apos;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

size_t append_body(char* ptr, size_t size, size_t count, void* context) {
  auto& body = *static_cast<std::string*>(context);
  const size_t bytes = size * count;
  if (body.size() + bytes > 8 * 1024 * 1024) return 0;
  body.append(ptr, bytes);
  return bytes;
}

class Xml {
 public:
  explicit Xml(const std::string& text)
      : doc_(xmlReadMemory(text.data(), static_cast<int>(text.size()), nullptr, nullptr, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), &xmlFreeDoc) {
    if (!doc_) throw std::runtime_error("invalid ONVIF XML response");
  }

  std::vector<xmlNodePtr> find(const char* expression, xmlNodePtr relative = nullptr) const {
    std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)> ctx(xmlXPathNewContext(doc_.get()), &xmlXPathFreeContext);
    if (!ctx) throw std::runtime_error("ONVIF XML XPath setup failed");
    if (relative) ctx->node = relative;
    std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)> result(
      xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(expression), ctx.get()), &xmlXPathFreeObject);
    std::vector<xmlNodePtr> nodes;
    if (result && result->nodesetval) {
      for (int i = 0; i < result->nodesetval->nodeNr; ++i) nodes.push_back(result->nodesetval->nodeTab[i]);
    }
    return nodes;
  }

  static std::string value(xmlNodePtr node) {
    if (!node) return {};
    xmlChar* data = xmlNodeGetContent(node);
    std::string result = data ? reinterpret_cast<const char*>(data) : "";
    if (data) xmlFree(data);
    return result;
  }

  static std::string attribute(xmlNodePtr node, const char* name) {
    xmlChar* data = xmlGetProp(node, reinterpret_cast<const xmlChar*>(name));
    std::string result = data ? reinterpret_cast<const char*>(data) : "";
    if (data) xmlFree(data);
    return result;
  }

 private:
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc_;
};

class Fd {
 public:
  explicit Fd(int value = -1) : value_(value) {}
  ~Fd() { if (value_ >= 0) close(value_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return value_; }
 private:
  int value_;
};

void send_all(int fd, const std::string& data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t count = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (count <= 0) throw std::runtime_error("RTSP talk connection closed");
    offset += static_cast<size_t>(count);
  }
}

std::string rtsp_exchange(int fd, const std::string& request) {
  send_all(fd, request);
  std::string response;
  std::array<char, 4096> buffer{};
  size_t complete_size = 0;
  while (response.size() < 65536) {
    const ssize_t count = recv(fd, buffer.data(), buffer.size(), 0);
    if (count <= 0) throw std::runtime_error("RTSP talk response unavailable");
    response.append(buffer.data(), static_cast<size_t>(count));
    const size_t end = response.find("\r\n\r\n");
    if (end == std::string::npos) continue;
    if (!complete_size) {
      complete_size = end + 4;
      const size_t header = response.find("Content-Length:");
      if (header != std::string::npos && header < end)
        complete_size += static_cast<size_t>(std::stoul(response.substr(header + 15)));
    }
    if (response.size() >= complete_size) return response;
  }
  throw std::runtime_error("RTSP talk response too large");
}

std::string rtsp_header(const std::string& response, const std::string& name) {
  const size_t start = response.find(name + ":");
  if (start == std::string::npos) return {};
  const size_t begin = start + name.size() + 1;
  const size_t end = response.find("\r\n", begin);
  const size_t first = response.find_first_not_of(' ', begin);
  if (first == std::string::npos || first >= end) return {};
  return response.substr(first, end - first);
}

std::string quoted_param(const std::string& header, const std::string& name) {
  const size_t start = header.find(name + "=\"");
  if (start == std::string::npos) return {};
  const size_t begin = start + name.size() + 2;
  const size_t end = header.find('"', begin);
  return end == std::string::npos ? "" : header.substr(begin, end - begin);
}

std::string md5hex(const std::string& input) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (!EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_md5(), nullptr))
    throw std::runtime_error("RTSP digest failed");
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < length; ++i) out << std::setw(2) << static_cast<int>(digest[i]);
  return out.str();
}

class Camera {
 public:
  Camera() : Camera(load_settings()) {}
  explicit Camera(CameraSettings settings) : host_(std::move(settings.host)), user_(std::move(settings.user)), password_(std::move(settings.password)) {
    if (host_.empty()) throw std::runtime_error("camera not configured; run tplink-ipc-mcp configure");
    if (!std::all_of(host_.begin(), host_.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        })) throw std::runtime_error("camera address must be a hostname or IPv4 address without a port");
    base_ = "http://" + host_;
  }

  Json get(std::string module, std::vector<std::string> sections) {
    Json query = {{"method", "get"}, {module, {{"name", sections}}}};
    return request(query).at(module);
  }

  Json set(std::string module, std::string section, Json value) {
    return request({{"method", "set"}, {module, {{section, value}}}});
  }

  Json action(std::string module, std::string name, Json value) {
    return request({{"method", "do"}, {module, {{name, value}}}});
  }

  Json activation_status() {
    const auto [status, body] = http(base_ + "/pc/Content.htm", nullptr);
    if (status != 401) throw std::runtime_error("camera activation status unavailable");
    const Json response = Json::parse(body);
    const int code = response.at("data").at("code").get<int>();
    if (code == -40414) return {{"state", "needs_activation"}, {"username", "admin"}};
    if (code == -40405) return {{"state", "needs_password_change"}, {"username", "admin"}};
    if (code == -40407) return {{"state", "activated"}, {"username", "admin"},
      {"credentials_configured", !user_.empty() && !password_.empty()}};
    throw std::runtime_error("unknown camera activation status " + std::to_string(code));
  }

  Json manual_alarm(int seconds) {
    if (seconds < 1 || seconds > 5) throw std::runtime_error("alarm duration must be 1-5 seconds");
    try {
      action("msg_alarm", "manual_msg_alarm", {{"action", "start"}});
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
    } catch (...) {
      try { stop_alarm(); } catch (...) {}
      throw;
    }
    stop_alarm();
    return {{"triggered", true}, {"seconds", seconds}, {"stopped", true}};
  }

  void stop_alarm() {
    action("msg_alarm", "manual_msg_alarm", {{"action", "stop"}});
  }

  Json wait_person_alarm(int wait_seconds) {
    if (wait_seconds < 1 || wait_seconds > 30) throw std::runtime_error("wait_seconds must be 1-30");
    const Json detector = get("people_detection", {"detection"}).at("detection");
    if (detector.value("enabled", "off") != "on") throw std::runtime_error("person detection is disabled");
    const Json person = get("people_detection", {"notify_list"}).at("notify_list");
    const Json motion = get("motion_detection", {"motion_notif_list"}).at("motion_notif_list");
    if (person.value("sound_alarm_enabled", "off") != "off" || motion.value("sound_alarm_enabled", "off") != "off")
      throw std::runtime_error("sound linkage must be off for silent person monitoring");
    for (const auto& [module, section] : std::array<std::pair<const char*, const char*>, 6>{{
           {"tamper_detection", "tamper_notif_list"}, {"linecrossing_detection", "notify_list"},
           {"intrusion_detection", "notify_list"}, {"regionexiting_detection", "notify_list"},
           {"cry_detection", "notify_list"}, {"unusual_detection", "login_error"}}}) {
      const Json linkage = get(module, {section}).at(section);
      if (linkage.value("light_alarm_enabled", "off") == "on")
        throw std::runtime_error(std::string(module) + " also triggers the white light; cannot identify a person alarm");
      if (linkage.value("sound_alarm_enabled", "off") == "on")
        throw std::runtime_error(std::string(module) + " sound linkage must be off for silent monitoring");
    }

    bool changed_motion = false, changed_person = false;
    const auto restore = [&] {
      if (changed_person) {
        set("people_detection", "notify_list", {{"light_alarm_enabled", person.at("light_alarm_enabled")}});
        changed_person = false;
      }
      if (changed_motion) {
        set("motion_detection", "motion_notif_list", {{"light_alarm_enabled", motion.at("light_alarm_enabled")}});
        changed_motion = false;
      }
    };
    try {
      if (motion.value("light_alarm_enabled", "off") != "off") {
        set("motion_detection", "motion_notif_list", {{"light_alarm_enabled", "off"}});
        changed_motion = true;
      }
      if (person.value("light_alarm_enabled", "off") != "on") {
        set("people_detection", "notify_list", {{"light_alarm_enabled", "on"}});
        changed_person = true;
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_seconds);
      Json answer = {{"detected", false}, {"signal", "person_only_white_light"}};
      while (std::chrono::steady_clock::now() < deadline) {
        const Json state = get("image", {"light_status"}).at("light_status");
        if (state.value("wl_status", "off") == "blink") {
          answer["detected"] = true;
          answer["observed_at_unix"] = std::time(nullptr);
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
      restore();
      return answer;
    } catch (...) {
      try { restore(); } catch (...) {}
      throw;
    }
  }

  Json storage() {
    const Json result = request({{"method", "get"}, {"harddisk_manage", {{"name", Json::array({"harddisk"})}, {"table", Json::array({"hd_info"})}}}});
    const Json disks = result.at("harddisk_manage").at("hd_info");
    Json summary = Json::array();
    for (const auto& disk : disks) {
      for (auto it = disk.begin(); it != disk.end(); ++it) {
        const Json& info = it.value();
        summary.push_back({{"status", info.value("status", "unknown")}, {"total", info.value("total_space", "")},
                           {"free", info.value("free_space", "")}});
      }
    }
    return summary;
  }

  std::string snapshot(bool high_quality, int zoom, double focus_x, double focus_y) const {
    if (zoom < 1 || zoom > 5 || !std::isfinite(focus_x) || !std::isfinite(focus_y) ||
        focus_x < 0 || focus_x > 1 || focus_y < 0 || focus_y > 1)
      throw std::runtime_error("zoom must be 1-5 and focus_x/focus_y must be 0-1");
    const std::string stream = high_quality || zoom > 1 ? "stream1" : "stream2";
    const std::string url = "rtsp://" + url_encode(user_) + ":" + url_encode(password_) + "@" + host_ + ":554/" + stream;
    const std::string filter = "crop=iw/" + std::to_string(zoom) + ":ih/" + std::to_string(zoom) +
      ":(iw-iw/" + std::to_string(zoom) + ")*" + std::to_string(focus_x) +
      ":(ih-ih/" + std::to_string(zoom) + ")*" + std::to_string(focus_y);
    int fds[2];
    if (pipe(fds) != 0) throw std::runtime_error("snapshot pipe failed");
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); throw std::runtime_error("snapshot fork failed"); }
    if (pid == 0) {
      dup2(fds[1], STDOUT_FILENO);
      int null_fd = open_null();
      if (null_fd >= 0) { dup2(null_fd, STDERR_FILENO); close(null_fd); }
      close(fds[0]); close(fds[1]);
      if (zoom > 1)
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-rtsp_transport", "tcp", "-i", url.c_str(),
               "-vf", filter.c_str(), "-frames:v", "1", "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1", nullptr);
      else
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-rtsp_transport", "tcp", "-i", url.c_str(),
               "-frames:v", "1", "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1", nullptr);
      _exit(127);
    }
    close(fds[1]);
    std::string bytes;
    std::array<char, 8192> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (true) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) { kill(pid, SIGKILL); close(fds[0]); waitpid(pid, nullptr, 0); throw std::runtime_error("RTSP snapshot timed out"); }
      pollfd fd{fds[0], POLLIN, 0};
      if (poll(&fd, 1, static_cast<int>(remaining)) <= 0) { kill(pid, SIGKILL); close(fds[0]); waitpid(pid, nullptr, 0); throw std::runtime_error("RTSP snapshot timed out"); }
      const ssize_t n = read(fds[0], buffer.data(), buffer.size());
      if (n <= 0) break;
      if (bytes.size() + static_cast<size_t>(n) > 4 * 1024 * 1024) { kill(pid, SIGKILL); close(fds[0]); waitpid(pid, nullptr, 0); throw std::runtime_error("snapshot exceeds 4 MiB"); }
      bytes.append(buffer.data(), static_cast<size_t>(n));
    }
    close(fds[0]);
    int status = 0; waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || bytes.empty()) throw std::runtime_error("RTSP snapshot failed");
    return base64(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  }

  Json record_clip(int seconds, bool high_quality) const {
    if (seconds < 1 || seconds > 20) throw std::runtime_error("clip duration must be 1-20 seconds");
    const std::filesystem::path directory = std::filesystem::canonical(env("IPC_MEDIA_DIR"));
    if (!std::filesystem::is_directory(directory)) throw std::runtime_error("IPC_MEDIA_DIR must be a directory");
    std::array<unsigned char, 4> suffix{};
    if (RAND_bytes(suffix.data(), suffix.size()) != 1) throw std::runtime_error("recording filename generation failed");
    std::ostringstream name;
    name << "ipc-" << std::time(nullptr) << '-' << std::hex << std::setfill('0');
    for (unsigned char c : suffix) name << std::setw(2) << static_cast<int>(c);
    const std::filesystem::path output = directory / (name.str() + ".mkv");
    const std::string stream = high_quality ? "stream1" : "stream2";
    const std::string url = "rtsp://" + url_encode(user_) + ":" + url_encode(password_) + "@" + host_ + ":554/" + stream;
    const std::string duration = std::to_string(seconds);
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("recording fork failed");
    if (child == 0) {
      const int null_fd = open_null();
      if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); close(null_fd); }
      execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-rtsp_transport", "tcp", "-i", url.c_str(),
             "-t", duration.c_str(), "-map", "0:v:0", "-map", "0:a?", "-c", "copy", "-y", output.c_str(), nullptr);
      _exit(127);
    }
    int status = 0;
    bool done = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds + 10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (waitpid(child, &status, WNOHANG) == child) { done = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!done) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
    if (!done || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || !std::filesystem::exists(output) || std::filesystem::file_size(output) == 0) {
      std::filesystem::remove(output);
      throw std::runtime_error("RTSP recording failed or timed out");
    }
    return {{"path", output.string()}, {"bytes", std::filesystem::file_size(output)}, {"seconds", seconds}, {"quality", high_quality ? "main" : "sub"}};
  }

  void relative_move(double pan, double tilt) {
    if (!std::isfinite(pan) || !std::isfinite(tilt) || pan < -0.2 || pan > 0.2 || tilt < -0.2 || tilt > 0.2 || (pan == 0 && tilt == 0))
      throw std::runtime_error("pan/tilt must be within [-0.2, 0.2] and at least one nonzero");
    soap("http://www.onvif.org/ver20/ptz/wsdl/RelativeMove", "<tptz:RelativeMove><tptz:ProfileToken>profile_1</tptz:ProfileToken><tptz:Translation><tt:PanTilt x=\"" +
      std::to_string(pan) + "\" y=\"" + std::to_string(tilt) + "\"/></tptz:Translation></tptz:RelativeMove>");
  }

  Json ptz_status() {
    const Xml xml(soap("http://www.onvif.org/ver20/ptz/wsdl/GetStatus",
      "<tptz:GetStatus><tptz:ProfileToken>profile_1</tptz:ProfileToken></tptz:GetStatus>"));
    const auto pan_tilt = xml.find("//*[local-name()='Position']/*[local-name()='PanTilt']");
    if (pan_tilt.empty()) throw std::runtime_error("ONVIF PTZ position missing");
    return {{"pan", std::stod(Xml::attribute(pan_tilt.front(), "x"))},
            {"tilt", std::stod(Xml::attribute(pan_tilt.front(), "y"))}};
  }

  void absolute_move(double pan, double tilt) {
    if (!std::isfinite(pan) || !std::isfinite(tilt) || pan < -1 || pan > 1 || tilt < -1 || tilt > 1)
      throw std::runtime_error("pan/tilt must be within [-1, 1]");
    soap("http://www.onvif.org/ver20/ptz/wsdl/AbsoluteMove",
      "<tptz:AbsoluteMove><tptz:ProfileToken>profile_1</tptz:ProfileToken><tptz:Position><tt:PanTilt x=\"" +
      std::to_string(pan) + "\" y=\"" + std::to_string(tilt) + "\"/></tptz:Position></tptz:AbsoluteMove>");
  }

  void remove_preset(std::string_view id) {
    soap("http://www.onvif.org/ver20/ptz/wsdl/RemovePreset", "<tptz:RemovePreset><tptz:ProfileToken>profile_1</tptz:ProfileToken><tptz:PresetToken>" +
      xml_escape(id) + "</tptz:PresetToken></tptz:RemovePreset>");
  }

  Json poll_events(int wait_seconds) {
    if (wait_seconds < 1 || wait_seconds > 5) throw std::runtime_error("wait_seconds must be 1-5");
    constexpr const char* create_action = "http://www.onvif.org/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest";
    const std::string create_response = soap(create_action,
      "<tev:CreatePullPointSubscription><tev:InitialTerminationTime>PT60S</tev:InitialTerminationTime></tev:CreatePullPointSubscription>");
    const Xml create_xml(create_response);
    const auto address_nodes = create_xml.find("//*[local-name()='SubscriptionReference']/*[local-name()='Address']");
    if (address_nodes.empty()) throw std::runtime_error("ONVIF event subscription address missing");
    const std::string address = Xml::value(address_nodes.front());
    if (address.rfind(base_ + ":", 0) != 0) throw std::runtime_error("ONVIF event subscription address has unexpected host");
    try {
      constexpr const char* pull_action = "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest";
      const std::string response = soap(pull_action,
        "<tev:PullMessages><tev:Timeout>PT" + std::to_string(wait_seconds) +
        "S</tev:Timeout><tev:MessageLimit>10</tev:MessageLimit></tev:PullMessages>", address);
      const Xml xml(response);
      Json events = Json::array();
      for (auto* notification : xml.find("//*[local-name()='NotificationMessage']")) {
        Json event = {{"topic", ""}, {"data", Json::object()}};
        const auto topics = xml.find(".//*[local-name()='Topic']", notification);
        if (!topics.empty()) event["topic"] = Xml::value(topics.front());
        for (auto* message : xml.find(".//*[local-name()='Message']", notification)) {
          const std::string utc_time = Xml::attribute(message, "UtcTime");
          if (!utc_time.empty()) { event["utc_time"] = utc_time; break; }
        }
        for (auto* item : xml.find(".//*[local-name()='SimpleItem']", notification)) {
          const std::string key = Xml::attribute(item, "Name");
          if (!key.empty()) event["data"][key] = Xml::attribute(item, "Value");
        }
        events.push_back(std::move(event));
      }
      try { soap("http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest", "<wsnt:Unsubscribe/>", address); }
      catch (...) {}
      return events;
    } catch (...) {
      try { soap("http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest", "<wsnt:Unsubscribe/>", address); }
      catch (...) {}
      throw;
    }
  }

  Json play_audio(const std::string& filename) {
    if (filename.empty() || filename.size() > 120 || filename[0] == '.' ||
        !std::all_of(filename.begin(), filename.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; }))
      throw std::runtime_error("audio filename must be a simple basename");
    const std::filesystem::path dir = std::filesystem::canonical(env("IPC_AUDIO_DIR"));
    const std::filesystem::path file = std::filesystem::canonical(dir / filename);
    if (file.parent_path() != dir || !std::filesystem::is_regular_file(file) ||
        std::filesystem::file_size(file) > 10 * 1024 * 1024)
      throw std::runtime_error("audio file is outside IPC_AUDIO_DIR or exceeds 10 MiB");
    const std::string ext = file.extension().string();
    if (ext != ".wav" && ext != ".mp3" && ext != ".m4a" && ext != ".ogg")
      throw std::runtime_error("unsupported audio file extension");

    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw = nullptr;
    if (getaddrinfo(host_.c_str(), "554", &hints, &raw) != 0) throw std::runtime_error("camera RTSP DNS lookup failed");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> address(raw, &freeaddrinfo);
    Fd tcp(socket(address->ai_family, address->ai_socktype, address->ai_protocol));
    if (tcp.get() < 0 || connect(tcp.get(), address->ai_addr, address->ai_addrlen) != 0)
      throw std::runtime_error("camera RTSP connection failed");
    timeval timeout{5, 0};
    setsockopt(tcp.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(tcp.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::array<unsigned char, 16> uuid_bytes{};
    if (RAND_bytes(uuid_bytes.data(), uuid_bytes.size()) != 1) throw std::runtime_error("talk UUID generation failed");
    std::ostringstream uuid_builder;
    uuid_builder << std::hex << std::setfill('0');
    for (size_t i = 0; i < uuid_bytes.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) uuid_builder << '-';
      uuid_builder << std::setw(2) << static_cast<int>(uuid_bytes[i]);
    }
    const std::string uuid = uuid_builder.str();
    const std::string uri = "rtsp://" + host_ + "/multitrans";
    const std::string prefix = "MULTITRANS " + uri + " RTSP/1.0\r\n";
    std::string response = rtsp_exchange(tcp.get(), prefix + "CSeq: 0\r\nX-Client-UUID: " + uuid + "\r\n\r\n");
    std::string session;
    if (response.rfind("RTSP/1.0 401", 0) == 0) {
      const std::string challenge = rtsp_header(response, "WWW-Authenticate");
      std::string digest_header = challenge;
      if (digest_header.rfind("Digest", 0) != 0) {
        const size_t start = response.find("WWW-Authenticate: Digest");
        if (start == std::string::npos) throw std::runtime_error("RTSP Digest challenge missing");
        const size_t begin = start + std::string("WWW-Authenticate: ").size();
        digest_header = response.substr(begin, response.find("\r\n", begin) - begin);
      }
      const std::string realm = quoted_param(digest_header, "realm");
      const std::string nonce = quoted_param(digest_header, "nonce");
      if (realm.empty() || nonce.empty()) throw std::runtime_error("RTSP Digest fields missing");
      const std::string digest = md5hex(md5hex(user_ + ":" + realm + ":" + password_) + ":" + nonce + ":" + md5hex("MULTITRANS:" + uri));
      const std::string authorization = "Digest username=\"" + user_ + "\", realm=\"" + realm + "\", nonce=\"" + nonce +
        "\", uri=\"" + uri + "\", response=\"" + digest + "\"";
      response = rtsp_exchange(tcp.get(), prefix + "CSeq: 1\r\nAuthorization: " + authorization + "\r\nX-Client-UUID: " + uuid + "\r\n\r\n");
    }
    if (response.rfind("RTSP/1.0 200", 0) != 0) throw std::runtime_error("RTSP talk authentication rejected");
    session = rtsp_header(response, "Session");
    if (session.empty()) throw std::runtime_error("RTSP talk session missing");
    const std::string payload = Json({{"type", "request"}, {"seq", 0}, {"params", {{"method", "get"}, {"talk", {{"mode", "half_duplex"}}}}}}).dump();
    response = rtsp_exchange(tcp.get(), prefix + "CSeq: 2\r\nSession: " + session +
      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload);
    if (response.rfind("RTSP/1.0 200", 0) != 0) throw std::runtime_error("RTSP talk setup rejected");
    const size_t body_start = response.find("\r\n\r\n");
    const Json talk = Json::parse(response.substr(body_start + 4));
    if (talk.at("params").value("error_code", -1) != 0) throw std::runtime_error("RTSP talk channel rejected");

    Fd udp(socket(AF_INET, SOCK_DGRAM, 0));
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK); local.sin_port = 0;
    if (udp.get() < 0 || bind(udp.get(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
      throw std::runtime_error("local RTP socket failed");
    socklen_t local_len = sizeof(local);
    if (getsockname(udp.get(), reinterpret_cast<sockaddr*>(&local), &local_len) != 0)
      throw std::runtime_error("local RTP port unavailable");
    const std::string target = "rtp://127.0.0.1:" + std::to_string(ntohs(local.sin_port));
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("ffmpeg fork failed");
    if (child == 0) {
      const int null_fd = open_null();
      if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); close(null_fd); }
      execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-re", "-i", file.c_str(),
             "-vn", "-ac", "1", "-ar", "8000", "-c:a", "pcm_alaw", "-f", "rtp", target.c_str(), nullptr);
      _exit(127);
    }
    int packets = 0, idle = 0, child_status = 0;
    bool done = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    try {
      while (std::chrono::steady_clock::now() < deadline) {
        pollfd ready{udp.get(), POLLIN, 0};
        if (poll(&ready, 1, 250) > 0 && (ready.revents & POLLIN)) {
          std::array<char, 4096> packet{};
          const ssize_t bytes = recv(udp.get(), packet.data(), packet.size(), 0);
          if (bytes > 0) {
            std::string frame; frame.reserve(static_cast<size_t>(bytes) + 4);
            frame += '$'; frame += static_cast<char>(1);
            frame += static_cast<char>((bytes >> 8) & 255); frame += static_cast<char>(bytes & 255);
            frame.append(packet.data(), static_cast<size_t>(bytes));
            send_all(tcp.get(), frame);
            ++packets; idle = 0;
          }
        } else ++idle;
        if (!done && waitpid(child, &child_status, WNOHANG) == child) done = true;
        if (done && idle >= 4) break;
      }
    } catch (...) {
      if (!done) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
      throw;
    }
    if (!done) { kill(child, SIGKILL); waitpid(child, nullptr, 0); throw std::runtime_error("audio playback exceeded 30 seconds"); }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0 || packets == 0)
      throw std::runtime_error("audio RTP playback failed");
    return {{"sent_rtp_packets", packets}};
  }

 private:
  static int open_null() { return open("/dev/null", O_WRONLY); }

  std::string soap(const std::string& action, const std::string& content, const std::string& endpoint = "") {
    std::array<unsigned char, 16> nonce{};
    if (RAND_bytes(nonce.data(), nonce.size()) != 1) throw std::runtime_error("random nonce failed");
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{}; gmtime_r(&now, &utc);
    char created[32]; std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::string digest_input(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    digest_input += created; digest_input += password_;
    std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
    SHA1(reinterpret_cast<const unsigned char*>(digest_input.data()), digest_input.size(), digest.data());
    const std::string soap =
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" xmlns:tt=\"http://www.onvif.org/ver10/schema\" xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
      "<s:Header><wsse:Security><wsse:UsernameToken><wsse:Username>" + xml_escape(user_) +
      "</wsse:Username><wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" +
      base64(digest.data(), digest.size()) + "</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" +
      base64(nonce.data(), nonce.size()) + "</wsse:Nonce><wsu:Created>" + created + "</wsu:Created></wsse:UsernameToken></wsse:Security></s:Header>"
      "<s:Body>" + content + "</s:Body></s:Envelope>";
    const auto [code, body] = http(endpoint.empty() ? base_ + ":2020/onvif/service" : endpoint, &soap,
      {"Content-Type: application/soap+xml; charset=utf-8; action=\"" + action + "\"",
       "SOAPAction: \"" + action + "\""});
    if (code != 200 || body.find("Fault") != std::string::npos) throw std::runtime_error("ONVIF request failed (HTTP " + std::to_string(code) + ")");
    return body;
  }

  std::pair<long, std::string> http(const std::string& url, const std::string* post,
                                    const std::vector<std::string>& headers = {}) const {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl initialization failed");
    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "TP-LINK_APP");
    curl_slist* raw = nullptr;
    for (const auto& h : headers) raw = curl_slist_append(raw, h.c_str());
    if (post) {
      if (headers.empty()) raw = curl_slist_append(raw, "Content-Type: application/json");
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, post->data());
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(post->size()));
    }
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, raw);
    const CURLcode result = curl_easy_perform(curl.get());
    curl_slist_free_all(raw);
    if (result != CURLE_OK) throw std::runtime_error(std::string("camera HTTP error: ") + curl_easy_strerror(result));
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    return {status, body};
  }

  static std::string web_password(std::string_view password) {
    constexpr std::string_view salt = "RDpbLfCPsJZ7fiv";
    constexpr std::string_view alphabet = "yLwVl0zKqws7LgKPRQ84Mdt708T1qQ3Ha7xv3H7NyU84p21BriUWBU43odz3iP4rBL3cD02KZciXTysVXiV8ngg6vL48rPJyAUw0HurW20xqxv9aYb4M9wK1Ae0wlro510qXeU07kV57fQMc8L6aLgMLwygtc0F10a0Dg70TOoouyFhdysuRMO51yY5ZlOZZLEal1h0t9YQW0Ko7oBwmCAHoic4HYbUyVeU3sfQ1xtXcPcf1aT303wAQhv66qzW";
    std::u16string utf16;
    try { utf16 = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(password.data(), password.data() + password.size()); }
    catch (const std::range_error&) { throw std::runtime_error("password is not valid UTF-8"); }
    std::string result;
    for (size_t i = 0; i < std::max(utf16.size(), salt.size()); ++i) {
      const int a = i < utf16.size() ? utf16[i] : 187;
      const int b = i < salt.size() ? static_cast<unsigned char>(salt[i]) : 187;
      result += alphabet[static_cast<size_t>((a ^ b) % alphabet.size())];
    }
    return result;
  }

  void login() {
    if (user_.empty() || password_.empty()) throw std::runtime_error("camera credentials missing; run tplink-ipc-mcp configure");
    const auto [challenge_code, challenge_body] = http(base_ + "/pc/Content.htm", nullptr);
    if (challenge_code != 401) throw std::runtime_error("camera auth challenge unavailable");
    const Json challenge = Json::parse(challenge_body).at("data");
    const auto key_bytes = unbase64(url_decode(challenge.at("key").get<std::string>()));
    const unsigned char* key_ptr = key_bytes.data();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(d2i_PUBKEY(nullptr, &key_ptr, static_cast<long>(key_bytes.size())), &EVP_PKEY_free);
    if (!key) throw std::runtime_error("camera RSA public key invalid");
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new(key.get(), nullptr), &EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_encrypt_init(ctx.get()) <= 0 || EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING) <= 0)
      throw std::runtime_error("RSA encryption setup failed");
    const std::string clear = web_password(password_) + ":" + challenge.at("nonce").get<std::string>();
    size_t length = 0;
    if (EVP_PKEY_encrypt(ctx.get(), nullptr, &length, reinterpret_cast<const unsigned char*>(clear.data()), clear.size()) <= 0)
      throw std::runtime_error("RSA encryption sizing failed");
    std::vector<unsigned char> ciphertext(length);
    if (EVP_PKEY_encrypt(ctx.get(), ciphertext.data(), &length, reinterpret_cast<const unsigned char*>(clear.data()), clear.size()) <= 0)
      throw std::runtime_error("RSA encryption failed");
    const Json payload = {{"method", "do"}, {"login", {{"username", user_}, {"password", base64(ciphertext.data(), length)}, {"encrypt_type", "2"}}}};
    const std::string encoded = payload.dump();
    const auto [code, body] = http(base_ + "/", &encoded);
    if (code != 200) throw std::runtime_error("camera login HTTP " + std::to_string(code));
    const Json result = Json::parse(body);
    if (result.value("error_code", -1) != 0 || !result.contains("stok")) throw std::runtime_error("camera login rejected");
    stok_ = url_decode(result.at("stok").get<std::string>());
  }

  Json request(const Json& payload) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (stok_.empty()) login();
      const std::string encoded = payload.dump();
      const auto [code, body] = http(base_ + "/stok=" + url_encode(stok_) + "/ds", &encoded);
      if (code == 401 || code == 403) { stok_.clear(); continue; }
      if (code != 200) throw std::runtime_error("camera API HTTP " + std::to_string(code));
      const Json result = Json::parse(body);
      const int error = result.value("error_code", -1);
      if (error == -40401) { stok_.clear(); continue; }
      if (error != 0) throw std::runtime_error("camera API error " + std::to_string(error));
      return result;
    }
    throw std::runtime_error("camera session expired after retry");
  }

  std::string host_, user_, password_, base_, stok_;
};

Json tool(std::string name, std::string description, Json properties = Json::object(), Json required = Json::array()) {
  return {{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}}}};
}

Json tools() {
  const Json boolean = {{"type", "boolean"}};
  return Json::array({
    tool("camera_activation_status", "Read whether the camera needs first-time activation. Requires only a camera address and does not change the device."),
    tool("camera_info", "Read camera model, firmware and supported local features."),
    tool("camera_status", "Read current human/motion/tamper detection, tracking, privacy, night vision, alarm, and SD card status."),
    tool("camera_events", "Wait briefly for ONVIF motion or tamper events. This firmware does not expose person classification through ONVIF.",
         {{"wait_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5}}}}),
    tool("camera_person_alarm", "Wait for the camera's person-only white-light alarm status. Temporarily isolates person light linkage, keeps sound off, and restores settings. A concurrent manual light alarm can mimic this signal.",
         {{"wait_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 30}}}}),
    tool("camera_snapshot", "Capture one current RTSP frame. Optional main-stream quality and 1-5x digital crop; focus_x/y are 0-1 crop positions.",
         {{"high_quality", {{"type", "boolean"}}}, {"zoom", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5}}},
          {"focus_x", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
          {"focus_y", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}),
    tool("camera_record_clip", "Record a short local MKV clip directly from RTSP without transcoding. Requires IPC_MEDIA_DIR and ffmpeg.",
         {{"seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}},
          {"high_quality", {{"type", "boolean"}}}}, {"seconds"}),
    tool("camera_play_audio", "Play a short audio file through the camera speaker using local RTSP talkback. Filename must be in IPC_AUDIO_DIR; requires ffmpeg.",
         {{"filename", {{"type", "string"}}}}, {"filename"}),
    tool("camera_alarm", "Sound the camera's manual alarm for 1-5 seconds, then stop it automatically.",
         {{"seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5}}}}, {"seconds"}),
    tool("camera_stop_alarm", "Stop the camera's manual alarm immediately."),
    tool("camera_ptz_status", "Read the camera's current absolute pan and tilt coordinates."),
    tool("camera_move", "Move pan and tilt by small relative fractions; range -0.2 to 0.2 per call.",
         {{"pan", {{"type", "number"}, {"minimum", -0.2}, {"maximum", 0.2}}}, {"tilt", {{"type", "number"}, {"minimum", -0.2}, {"maximum", 0.2}}}}, {"pan", "tilt"}),
    tool("camera_go_to_position", "Move the camera to absolute pan and tilt coordinates in [-1, 1].",
         {{"pan", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}}, {"tilt", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}}}, {"pan", "tilt"}),
    tool("camera_presets", "List saved camera PTZ positions."),
    tool("camera_save_preset", "Save the camera's current pan/tilt position with a name.", {{"name", {{"type", "string"}, {"maxLength", 32}}}}, {"name"}),
    tool("camera_go_to_preset", "Move camera to an existing preset ID.", {{"id", {{"type", "string"}}}}, {"id"}),
    tool("camera_remove_preset", "Remove an existing saved preset by ID.", {{"id", {{"type", "string"}}}}, {"id"}),
    tool("camera_set_privacy", "Enable or disable physical lens privacy mode.", {{"enabled", boolean}}, {"enabled"}),
    tool("camera_set_led", "Enable or disable the camera status LED.", {{"enabled", boolean}}, {"enabled"}),
    tool("camera_set_speaker_volume", "Set the camera speaker volume from 0 to 100.",
         {{"volume", {{"type", "integer"}, {"minimum", 0}, {"maximum", 100}}}}, {"volume"}),
    tool("camera_set_tracking", "Enable or disable the camera's on-device target tracking.", {{"enabled", boolean}}, {"enabled"}),
    tool("camera_set_detection", "Enable or disable one on-device detector.",
         {{"kind", {{"type", "string"}, {"enum", {"person", "motion", "tamper"}}}}, {"enabled", boolean}}, {"kind", "enabled"}),
    tool("camera_set_night_vision", "Set local infrared, white light or motion-activated night vision mode.",
         {{"mode", {{"type", "string"}, {"enum", {"inf_night_vision", "wtl_night_vision", "md_night_vision"}}}}}, {"mode"})
  });
}

Json text_result(const Json& value) { return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}}; }

Json call(Camera& camera, const std::string& name, const Json& args) {
  if (name == "camera_activation_status") return text_result(camera.activation_status());
  if (name == "camera_info") {
    const Json basic = camera.get("device_info", {"basic_info"}).at("basic_info");
    const Json modules = camera.get("function", {"module_spec"}).at("module_spec");
    return text_result({{"model", basic.value("device_model", "")}, {"hardware", basic.value("hw_version", "")},
      {"firmware", url_decode(basic.value("sw_version", ""))}, {"alias", basic.value("device_alias", "")},
      {"capabilities", {{"ptz", modules.value("ptz", "0") == "1"}, {"human_detection", modules.value("smart_detection", "0") == "1"},
        {"tracking", modules.value("target_track", "0") == "1"}, {"privacy", modules.value("lens_mask", "0") == "1"}}}});
  }
  if (name == "camera_status") {
    Json result;
    result["person"] = camera.get("people_detection", {"detection"}).at("detection");
    result["motion"] = camera.get("motion_detection", {"motion_det"}).at("motion_det");
    result["tamper"] = camera.get("tamper_detection", {"tamper_det"}).at("tamper_det");
    result["tracking"] = camera.get("target_track", {"target_track_info"}).at("target_track_info");
    result["privacy"] = camera.get("lens_mask", {"lens_mask_info"}).at("lens_mask_info");
    const Json image = camera.get("image", {"switch"}).at("switch");
    result["night_vision"] = image.value("night_vision_mode", "");
    result["rotation"] = image.value("rotate_type", "");
    result["alarm"] = camera.get("msg_alarm", {"chn1_msg_alarm_info"}).at("chn1_msg_alarm_info");
    result["led"] = camera.get("led", {"config"}).at("config");
    result["speaker"] = camera.get("audio_config", {"speaker"}).at("speaker");
    result["storage"] = camera.storage();
    return text_result(result);
  }
  if (name == "camera_snapshot") return {{"content", Json::array({{{"type", "image"},
    {"data", camera.snapshot(args.value("high_quality", false), args.value("zoom", 1),
                             args.value("focus_x", 0.5), args.value("focus_y", 0.5))}, {"mimeType", "image/jpeg"}}})}};
  if (name == "camera_record_clip") return text_result(camera.record_clip(args.at("seconds").get<int>(), args.value("high_quality", false)));
  if (name == "camera_play_audio") return text_result(camera.play_audio(args.at("filename").get<std::string>()));
  if (name == "camera_alarm") return text_result(camera.manual_alarm(args.at("seconds").get<int>()));
  if (name == "camera_stop_alarm") {
    camera.stop_alarm();
    return text_result({{"stopped", true}});
  }
  if (name == "camera_ptz_status") return text_result(camera.ptz_status());
  if (name == "camera_events") return text_result(camera.poll_events(args.value("wait_seconds", 5)));
  if (name == "camera_person_alarm") return text_result(camera.wait_person_alarm(args.value("wait_seconds", 10)));
  if (name == "camera_move") {
    camera.relative_move(args.at("pan").get<double>(), args.at("tilt").get<double>());
    return text_result({{"moved", true}});
  }
  if (name == "camera_go_to_position") {
    camera.absolute_move(args.at("pan").get<double>(), args.at("tilt").get<double>());
    return text_result({{"moved", true}});
  }
  if (name == "camera_presets") return text_result(camera.get("preset", {"preset"}).at("preset"));
  if (name == "camera_save_preset") {
    const std::string preset_name = args.at("name").get<std::string>();
    if (preset_name.empty() || preset_name.size() > 32) throw std::runtime_error("preset name must be 1-32 bytes");
    return text_result(camera.action("preset", "set_preset", {{"name", preset_name}, {"save_ptz", "1"}}));
  }
  if (name == "camera_go_to_preset") {
    const std::string id = args.at("id").get<std::string>();
    if (id.empty() || id.size() > 16 || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); })) throw std::runtime_error("invalid preset ID");
    return text_result(camera.action("preset", "goto_preset", {{"id", id}}));
  }
  if (name == "camera_remove_preset") {
    const std::string id = args.at("id").get<std::string>();
    if (id.empty() || id.size() > 16 || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); })) throw std::runtime_error("invalid preset ID");
    camera.remove_preset(id);
    return text_result({{"removed", true}});
  }
  if (name == "camera_set_privacy") return text_result(camera.set("lens_mask", "lens_mask_info", {{"enabled", args.at("enabled").get<bool>() ? "on" : "off"}}));
  if (name == "camera_set_led") return text_result(camera.set("led", "config", {{"enabled", args.at("enabled").get<bool>() ? "on" : "off"}}));
  if (name == "camera_set_speaker_volume") {
    const int volume = args.at("volume").get<int>();
    if (volume < 0 || volume > 100) throw std::runtime_error("speaker volume must be 0-100");
    return text_result(camera.set("audio_config", "speaker", {{"volume", std::to_string(volume)}}));
  }
  if (name == "camera_set_tracking") return text_result(camera.set("target_track", "target_track_info", {{"enabled", args.at("enabled").get<bool>() ? "on" : "off"}}));
  if (name == "camera_set_detection") {
    const std::string kind = args.at("kind").get<std::string>();
    const std::string enabled = args.at("enabled").get<bool>() ? "on" : "off";
    if (kind == "person") return text_result(camera.set("people_detection", "detection", {{"enabled", enabled}}));
    if (kind == "motion") return text_result(camera.set("motion_detection", "motion_det", {{"enabled", enabled}}));
    if (kind == "tamper") return text_result(camera.set("tamper_detection", "tamper_det", {{"enabled", enabled}}));
    throw std::runtime_error("unknown detector");
  }
  if (name == "camera_set_night_vision") {
    const std::string mode = args.at("mode").get<std::string>();
    if (mode != "inf_night_vision" && mode != "wtl_night_vision" && mode != "md_night_vision") throw std::runtime_error("invalid night vision mode");
    return text_result(camera.set("image", "switch", {{"night_vision_mode", mode}}));
  }
  throw std::runtime_error("unknown tool");
}

int command_line(int argc, char** argv) {
  if (argc != 2) throw std::runtime_error("usage: tplink-ipc-mcp [configure|check]");
  const std::string command = argv[1];
  if (command == "--help" || command == "help") {
    std::cout << "Usage: tplink-ipc-mcp [configure|check]\n"
                 "  configure  Save camera address and credentials after verifying login\n"
                 "  check      Verify the saved camera connection\n"
                 "  no command starts the MCP stdio server\n";
    return 0;
  }
  if (command == "configure") {
    if (!isatty(STDIN_FILENO)) throw std::runtime_error("configure requires an interactive terminal");
    CameraSettings settings = load_settings();
    const std::string host = prompt("Camera IP or hostname" + (settings.host.empty() ? std::string(": ") : " [" + settings.host + "]: "));
    if (!host.empty()) settings.host = host;
    if (settings.host.empty()) throw std::runtime_error("camera address is required");
    Camera status_camera({settings.host, "", ""});
    const std::string state = status_camera.activation_status().at("state").get<std::string>();
    if (state != "activated") throw std::runtime_error("camera state is " + state + "; finish setup in the TP-LINK app or camera web page first");
    const std::string user = prompt("Camera username [admin]: ");
    settings.user = user.empty() ? "admin" : user;
    settings.password = prompt_password();
    if (settings.password.empty()) throw std::runtime_error("camera password is required");
    Camera camera(settings);
    const Json basic = camera.get("device_info", {"basic_info"}).at("basic_info");
    save_settings(settings);
    std::cout << "Connected to " << basic.value("device_model", "camera") << ". Configuration saved to " << config_path() << "\n";
    return 0;
  }
  if (command == "check") {
    Camera camera;
    const Json basic = camera.get("device_info", {"basic_info"}).at("basic_info");
    std::cout << "Ready: " << basic.value("device_model", "camera") << "\n";
    return 0;
  }
  throw std::runtime_error("unknown command; use --help");
}

}  // namespace

int main(int argc, char** argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  if (argc > 1) {
    try {
      const int result = command_line(argc, argv);
      curl_global_cleanup();
      return result;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      curl_global_cleanup();
      return 1;
    }
  }
  std::unique_ptr<Camera> camera;
  std::string line;
  while (std::getline(std::cin, line)) {
    Json input;
    try {
      input = Json::parse(line);
      if (!input.is_object() || !input.contains("method")) continue;
      const std::string method = input.at("method").get<std::string>();
      if (!input.contains("id")) continue;
      const Json id = input.at("id");
      Json result;
      if (method == "initialize") {
        const std::string requested = input.value("params", Json::object()).value("protocolVersion", "2025-11-25");
        const std::string version = requested == "2024-11-05" || requested == "2025-03-26" ||
          requested == "2025-06-18" || requested == "2025-11-25" ? requested : "2025-11-25";
        result = {{"protocolVersion", version}, {"capabilities", {{"tools", {{"listChanged", false}}}}},
                  {"serverInfo", {{"name", "tplink-ipc-mcp"}, {"version", TPLINK_IPC_VERSION}}}};
      } else if (method == "ping") result = Json::object();
      else if (method == "tools/list") result = {{"tools", tools()}};
      else if (method == "tools/call") {
        const Json params = input.at("params");
        try {
          if (!camera) camera = std::make_unique<Camera>();
          result = call(*camera, params.at("name").get<std::string>(), params.value("arguments", Json::object()));
        } catch (const std::exception& error) {
          result = {{"content", Json::array({{{"type", "text"}, {"text", error.what()}}})}, {"isError", true}};
        }
      } else {
        std::cout << Json({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "method not found"}}}}).dump() << '\n' << std::flush;
        continue;
      }
      std::cout << Json({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}).dump() << '\n' << std::flush;
    } catch (const std::exception& error) {
      if (input.is_object() && input.contains("id"))
        std::cout << Json({{"jsonrpc", "2.0"}, {"id", input["id"]}, {"error", {{"code", -32603}, {"message", error.what()}}}}).dump() << '\n' << std::flush;
    }
  }
  camera.reset();
  curl_global_cleanup();
}
