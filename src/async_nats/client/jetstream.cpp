#include <AsyncNats.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AsyncNats {
namespace {

struct ApiFailure {
  bool failed = false;
  int code = 0;
  int err_code = 0;
  std::string description;
};

auto jsonEscape(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 0x0f];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

auto skipWs(std::string_view json, std::size_t pos) -> std::size_t {
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
    ++pos;
  }
  return pos;
}

auto findKey(std::string_view json, std::string_view key) -> std::size_t {
  const auto pattern = std::string("\"") + std::string(key) + "\"";
  auto pos = json.find(pattern);
  if (pos == std::string_view::npos) {
    return std::string_view::npos;
  }
  pos = json.find(':', pos + pattern.size());
  if (pos == std::string_view::npos) {
    return std::string_view::npos;
  }
  return skipWs(json, pos + 1);
}

auto jsonString(std::string_view json, std::string_view key) -> std::optional<std::string> {
  auto pos = findKey(json, key);
  if (pos == std::string_view::npos || pos >= json.size() || json[pos] != '"') {
    return std::nullopt;
  }
  ++pos;
  std::string out;
  while (pos < json.size()) {
    const auto c = json[pos++];
    if (c == '"') {
      return out;
    }
    if (c != '\\' || pos >= json.size()) {
      out += c;
      continue;
    }
    const auto escaped = json[pos++];
    switch (escaped) {
      case '"':
      case '\\':
      case '/':
        out += escaped;
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u':
        if (pos + 4 <= json.size()) {
          pos += 4;
        }
        out += '?';
        break;
      default:
        out += escaped;
        break;
    }
  }
  return std::nullopt;
}

auto jsonUint(std::string_view json, std::string_view key) -> std::optional<std::uint64_t> {
  auto pos = findKey(json, key);
  if (pos == std::string_view::npos || pos >= json.size() || std::isdigit(static_cast<unsigned char>(json[pos])) == 0) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])) != 0) {
    value = value * 10 + static_cast<std::uint64_t>(json[pos] - '0');
    ++pos;
  }
  return value;
}

auto jsonBool(std::string_view json, std::string_view key) -> std::optional<bool> {
  auto pos = findKey(json, key);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  if (json.substr(pos, 4) == "true") {
    return true;
  }
  if (json.substr(pos, 5) == "false") {
    return false;
  }
  return std::nullopt;
}

auto readApiFailure(std::string_view json) -> ApiFailure {
  auto pos = findKey(json, "error");
  if (pos == std::string_view::npos || pos >= json.size() || json[pos] != '{') {
    return {};
  }
  const auto end = json.find('}', pos);
  const auto object = json.substr(pos, end == std::string_view::npos ? json.size() - pos : end - pos + 1);
  ApiFailure failure;
  failure.failed = true;
  if (auto code = jsonUint(object, "code")) {
    failure.code = static_cast<int>(*code);
  }
  if (auto err_code = jsonUint(object, "err_code")) {
    failure.err_code = static_cast<int>(*err_code);
  }
  if (auto description = jsonString(object, "description")) {
    failure.description = std::move(*description);
  } else {
    failure.description = "jetstream request failed";
  }
  return failure;
}

auto missingMessage(const ApiFailure& failure) -> bool {
  return failure.code == 404 || failure.err_code == 10037 || failure.err_code == 10059 || failure.err_code == 10014;
}

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

auto base64Encode(std::string_view input, bool url) -> std::string {
  const auto* alphabet = url ? kBase64Url : kBase64;
  std::string out;
  out.reserve((input.size() + 2) / 3 * 4);
  std::size_t index = 0;
  while (index + 3 <= input.size()) {
    const auto b0 = static_cast<unsigned char>(input[index]);
    const auto b1 = static_cast<unsigned char>(input[index + 1]);
    const auto b2 = static_cast<unsigned char>(input[index + 2]);
    out += alphabet[b0 >> 2];
    out += alphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
    out += alphabet[((b1 & 0x0f) << 2) | (b2 >> 6)];
    out += alphabet[b2 & 0x3f];
    index += 3;
  }
  if (index < input.size()) {
    const auto b0 = static_cast<unsigned char>(input[index]);
    out += alphabet[b0 >> 2];
    if (index + 1 == input.size()) {
      out += alphabet[(b0 & 0x03) << 4];
      out += '=';
      out += '=';
    } else {
      const auto b1 = static_cast<unsigned char>(input[index + 1]);
      out += alphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
      out += alphabet[(b1 & 0x0f) << 2];
      out += '=';
    }
  }
  return out;
}

auto base64Decode(std::string_view input) -> std::string {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') {
      return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
      return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
      return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
      return 62;
    }
    if (c == '/' || c == '_') {
      return 63;
    }
    return -1;
  };

  std::string out;
  out.reserve(input.size() / 4 * 3);
  int block[4];
  int count = 0;
  for (char c : input) {
    if (c == '=' || std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    const auto decoded = value(c);
    if (decoded < 0) {
      throw Error("invalid base64");
    }
    block[count++] = decoded;
    if (count == 4) {
      out += static_cast<char>((block[0] << 2) | (block[1] >> 4));
      out += static_cast<char>(((block[1] & 0x0f) << 4) | (block[2] >> 2));
      out += static_cast<char>(((block[2] & 0x03) << 6) | block[3]);
      count = 0;
    }
  }
  if (count == 2) {
    out += static_cast<char>((block[0] << 2) | (block[1] >> 4));
  } else if (count == 3) {
    out += static_cast<char>((block[0] << 2) | (block[1] >> 4));
    out += static_cast<char>(((block[1] & 0x0f) << 4) | (block[2] >> 2));
  }
  return out;
}

constexpr std::uint32_t kSha256[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

constexpr auto rotr(std::uint32_t value, std::uint32_t bits) -> std::uint32_t {
  return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
 public:
  Sha256() {
    state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  }

  void update(std::string_view data) {
    for (unsigned char byte : data) {
      block_[used_++] = byte;
      if (used_ == block_.size()) {
        transform(block_.data());
        bits_ += 512;
        used_ = 0;
      }
    }
  }

  auto digest() -> std::array<std::uint8_t, 32> {
    auto copy = *this;
    const auto total_bits = copy.bits_ + copy.used_ * 8;
    copy.block_[copy.used_++] = 0x80;
    if (copy.used_ > 56) {
      while (copy.used_ < 64) {
        copy.block_[copy.used_++] = 0;
      }
      copy.transform(copy.block_.data());
      copy.used_ = 0;
    }
    while (copy.used_ < 56) {
      copy.block_[copy.used_++] = 0;
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
      copy.block_[copy.used_++] = static_cast<std::uint8_t>((total_bits >> shift) & 0xff);
    }
    copy.transform(copy.block_.data());

    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < copy.state_.size(); ++i) {
      out[i * 4] = static_cast<std::uint8_t>(copy.state_[i] >> 24);
      out[i * 4 + 1] = static_cast<std::uint8_t>(copy.state_[i] >> 16);
      out[i * 4 + 2] = static_cast<std::uint8_t>(copy.state_[i] >> 8);
      out[i * 4 + 3] = static_cast<std::uint8_t>(copy.state_[i]);
    }
    return out;
  }

 private:
  void transform(const std::uint8_t* chunk) {
    std::uint32_t words[64];
    for (int i = 0; i < 16; ++i) {
      words[i] = (static_cast<std::uint32_t>(chunk[i * 4]) << 24) |
                 (static_cast<std::uint32_t>(chunk[i * 4 + 1]) << 16) |
                 (static_cast<std::uint32_t>(chunk[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(chunk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const auto s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
      const auto s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
      words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (int i = 0; i < 64; ++i) {
      const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temp1 = h + s1 + choose + kSha256[i] + words[i];
      const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> block_{};
  std::uint64_t bits_ = 0;
  std::size_t used_ = 0;
};

auto validBucket(std::string_view bucket) -> bool {
  if (bucket.empty()) {
    return false;
  }
  return std::all_of(bucket.begin(), bucket.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-';
  });
}

auto validKey(std::string_view key) -> bool {
  if (key.empty() || key.front() == '.' || key.back() == '.') {
    return false;
  }
  return std::all_of(key.begin(), key.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '/' || c == '=' || c == '.';
  });
}

auto requireBucket(std::string_view bucket) -> void {
  if (!validBucket(bucket)) {
    throw Error("invalid bucket name");
  }
}

auto requireKey(std::string_view key) -> void {
  if (!validKey(key)) {
    throw Error("invalid key");
  }
}

constexpr std::uint64_t kWrongLastSequence = 10071;
constexpr std::size_t kObjectChunkSize = 128 * 1024;

using Headers = std::vector<std::pair<std::string, std::string>>;

auto call(Client& connection, std::string subject, std::string payload, Headers fields = {})
    -> boost::cobalt::task<std::string> {
  co_return co_await connection.request(std::move(subject), std::move(payload), std::move(fields));
}

auto revisionOf(std::string_view ack) -> std::uint64_t {
  const auto failure = readApiFailure(ack);
  if (failure.failed) {
    throw Error(failure.description);
  }
  const auto sequence = jsonUint(ack, "seq");
  if (!sequence) {
    throw Error("jetstream publish was not acknowledged");
  }
  return *sequence;
}

struct StoredMessage {
  std::string subject;
  std::string data;
  std::string headers;
  std::uint64_t sequence = 0;
};

auto messageFromGet(std::string_view json) -> StoredMessage {
  StoredMessage message;
  if (auto sequence = jsonUint(json, "seq")) {
    message.sequence = *sequence;
  }
  if (auto subject = jsonString(json, "subject")) {
    message.subject = std::move(*subject);
  }
  if (auto data = jsonString(json, "data")) {
    message.data = base64Decode(*data);
  }
  if (auto header = jsonString(json, "hdrs")) {
    message.headers = base64Decode(*header);
  }
  return message;
}

enum class Operation { put, del, purge };

auto operationFromHeaders(std::string_view header_block) -> Operation {
  const auto marker = header_block.find("KV-Operation:");
  if (marker != std::string_view::npos) {
    auto value = header_block.substr(marker + std::string_view("KV-Operation:").size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    if (value.starts_with("PURGE")) {
      return Operation::purge;
    }
    if (value.starts_with("DEL")) {
      return Operation::del;
    }
  }
  if (header_block.find("Nats-Marker-Reason: Purge") != std::string_view::npos ||
      header_block.find("Nats-Marker-Reason: MaxAge") != std::string_view::npos) {
    return Operation::purge;
  }
  if (header_block.find("Nats-Marker-Reason: Remove") != std::string_view::npos) {
    return Operation::del;
  }
  return Operation::put;
}

auto newNuid() -> std::string {
  static constexpr char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  std::random_device device;
  std::uniform_int_distribution<int> pick(0, 61);
  std::string id(22, '0');
  for (char& c : id) {
    c = alphabet[pick(device)];
  }
  return id;
}

struct ObjectMeta {
  std::string name;
  std::string nuid;
  std::uint64_t size = 0;
  std::uint64_t chunks = 0;
  bool deleted = false;
};

auto metaFromJson(std::string_view json) -> ObjectMeta {
  ObjectMeta meta;
  if (auto name = jsonString(json, "name")) {
    meta.name = std::move(*name);
  }
  if (auto nuid = jsonString(json, "nuid")) {
    meta.nuid = std::move(*nuid);
  }
  if (auto size = jsonUint(json, "size")) {
    meta.size = *size;
  }
  if (auto chunks = jsonUint(json, "chunks")) {
    meta.chunks = *chunks;
  }
  if (auto deleted = jsonBool(json, "deleted")) {
    meta.deleted = *deleted;
  }
  return meta;
}

}  // namespace

jetstream::kv::Store::Store(Client client, std::string bucket) : client_(std::move(client)), bucket_(std::move(bucket)) {}

namespace {

auto kvSubject(std::string_view bucket, std::string_view key) -> std::string {
  return "$KV." + std::string(bucket) + "." + std::string(key);
}

}  // namespace

auto jetstream::kv::Store::put(std::string key, std::string value) -> boost::cobalt::task<std::uint64_t> {
  requireKey(key);
  const auto ack = co_await call(client_, kvSubject(bucket_, key), std::move(value));
  co_return revisionOf(ack);
}

auto jetstream::kv::Store::update(std::string key, std::string value, std::uint64_t revision) -> boost::cobalt::task<std::uint64_t> {
  requireKey(key);
  const auto ack = co_await call(client_, kvSubject(bucket_, key), std::move(value),
                                 {{"Nats-Expected-Last-Subject-Sequence", std::to_string(revision)}});
  co_return revisionOf(ack);
}

namespace {

struct KvRecord {
  std::string value;
  std::uint64_t revision = 0;
  Operation operation = Operation::put;
};

auto loadKey(Client& connection, std::string_view bucket, std::string_view key) -> boost::cobalt::task<std::optional<KvRecord>> {
  const auto subject = "$KV." + std::string(bucket) + "." + std::string(key);
  const auto response =
      co_await call(connection, "$JS.API.STREAM.MSG.GET.KV_" + std::string(bucket),
                    std::string("{\"last_by_subj\":\"") + jsonEscape(subject) + "\"}");
  const auto failure = readApiFailure(response);
  if (failure.failed) {
    if (missingMessage(failure)) {
      co_return std::nullopt;
    }
    throw Error(failure.description);
  }
  auto message = messageFromGet(response);
  co_return KvRecord{std::move(message.data), message.sequence, operationFromHeaders(message.headers)};
}

}  // namespace

auto jetstream::kv::Store::create(std::string key, std::string value) -> boost::cobalt::task<std::uint64_t> {
  requireKey(key);
  const auto subject = kvSubject(bucket_, key);
  const auto ack =
      co_await call(client_, subject, value, {{"Nats-Expected-Last-Subject-Sequence", "0"}});
  const auto failure = readApiFailure(ack);
  if (!failure.failed) {
    co_return revisionOf(ack);
  }
  if (failure.err_code != kWrongLastSequence) {
    throw Error(failure.description);
  }

  const auto current = co_await loadKey(client_, bucket_, key);
  if (!current || current->operation == Operation::put) {
    throw Error("key already exists");
  }
  co_return co_await update(std::move(key), std::move(value), current->revision);
}

auto jetstream::kv::Store::get(std::string key) -> boost::cobalt::task<std::optional<std::string>> {
  requireKey(key);
  const auto current = co_await loadKey(client_, bucket_, key);
  if (!current || current->operation != Operation::put) {
    co_return std::nullopt;
  }
  co_return current->value;
}

auto jetstream::kv::Store::entry(std::string key) -> boost::cobalt::task<std::optional<struct Entry>> {
  requireKey(key);
  const auto current = co_await loadKey(client_, bucket_, key);
  if (!current || current->operation != Operation::put) {
    co_return std::nullopt;
  }
  co_return jetstream::kv::Entry{std::move(key), current->value, current->revision};
}

auto jetstream::kv::Store::remove(std::string key) -> boost::cobalt::task<void> {
  requireKey(key);
  const auto ack = co_await call(client_, kvSubject(bucket_, key), "", {{"KV-Operation", "DEL"}});
  revisionOf(ack);
  co_return;
}

auto jetstream::kv::Store::purge(std::string key) -> boost::cobalt::task<void> {
  requireKey(key);
  const auto ack = co_await call(client_, kvSubject(bucket_, key), "",
                                 {{"KV-Operation", "PURGE"}, {"Nats-Rollup", "sub"}});
  revisionOf(ack);
  co_return;
}

jetstream::ObjectStore::Store::Store(Client client, std::string bucket)
    : client_(std::move(client)), bucket_(std::move(bucket)) {}

namespace {

auto metaSubject(std::string_view bucket, std::string_view name) -> std::string {
  return "$O." + std::string(bucket) + ".M." + base64Encode(name, true);
}

auto chunkSubject(std::string_view bucket, std::string_view nuid) -> std::string {
  return "$O." + std::string(bucket) + ".C." + std::string(nuid);
}

auto getMessage(Client& connection, std::string stream, std::string body)
    -> boost::cobalt::task<std::optional<StoredMessage>> {
  const auto response = co_await call(connection, "$JS.API.STREAM.MSG.GET." + stream, std::move(body));
  const auto failure = readApiFailure(response);
  if (failure.failed) {
    if (missingMessage(failure)) {
      co_return std::nullopt;
    }
    throw Error(failure.description);
  }
  co_return messageFromGet(response);
}

auto loadObject(Client& connection, std::string_view bucket, std::string_view name, bool allow_deleted)
    -> boost::cobalt::task<std::optional<ObjectMeta>> {
  const auto loaded =
      co_await getMessage(connection, "OBJ_" + std::string(bucket),
                           std::string("{\"last_by_subj\":\"") + jsonEscape(metaSubject(bucket, name)) + "\"}");
  if (!loaded) {
    co_return std::nullopt;
  }
  auto meta = metaFromJson(loaded->data);
  if (meta.name.empty()) {
    meta.name = std::string(name);
  }
  if (meta.deleted && !allow_deleted) {
    co_return std::nullopt;
  }
  co_return meta;
}

auto purgeSubject(Client& connection, std::string_view bucket, std::string_view subject) -> boost::cobalt::task<void> {
  const auto response = co_await call(connection, "$JS.API.STREAM.PURGE.OBJ_" + std::string(bucket),
                                      std::string("{\"filter\":\"") + jsonEscape(subject) + "\"}");
  const auto failure = readApiFailure(response);
  if (failure.failed) {
    throw Error(failure.description);
  }
  co_return;
}

auto publishMeta(Client& connection, std::string_view bucket, const ObjectMeta& meta, std::string digest)
    -> boost::cobalt::task<void> {
  const auto payload = std::string("{\"name\":\"") + jsonEscape(meta.name) + "\",\"bucket\":\"" + jsonEscape(bucket) +
                       "\",\"nuid\":\"" + jsonEscape(meta.nuid) + "\",\"size\":" + std::to_string(meta.size) +
                       ",\"chunks\":" + std::to_string(meta.chunks) + ",\"digest\":\"" + jsonEscape(digest) +
                       "\",\"deleted\":" + (meta.deleted ? "true" : "false") + "}";
  const auto ack = co_await call(connection, metaSubject(bucket, meta.name), payload, {{"Nats-Rollup", "sub"}});
  revisionOf(ack);
  co_return;
}

}  // namespace

auto jetstream::ObjectStore::Store::put(std::string name, std::string data) -> boost::cobalt::task<ObjectInfo> {
  if (name.empty()) {
    throw Error("object name is required");
  }
  const auto previous = co_await loadObject(client_, bucket_, name, true);
  const auto nuid = newNuid();
  const auto chunks_subject = chunkSubject(bucket_, nuid);

  Sha256 hash;
  std::uint64_t chunks = 0;
  std::exception_ptr chunk_failure;
  try {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const auto count = std::min(kObjectChunkSize, data.size() - offset);
      auto piece = data.substr(offset, count);
      hash.update(piece);
      const auto ack = co_await call(client_, chunks_subject, std::move(piece));
      revisionOf(ack);
      ++chunks;
      offset += count;
    }
  } catch (...) {
    chunk_failure = std::current_exception();
  }
  if (chunk_failure) {
    try {
      co_await purgeSubject(client_, bucket_, chunks_subject);
    } catch (...) {
    }
    std::rethrow_exception(chunk_failure);
  }

  auto digest_bytes = hash.digest();
  const auto digest =
      std::string("SHA-256=") + base64Encode(std::string_view(reinterpret_cast<const char*>(digest_bytes.data()),
                                                               digest_bytes.size()),
                                               true);
  ObjectMeta meta;
  meta.name = name;
  meta.nuid = nuid;
  meta.size = data.size();
  meta.chunks = chunks;
  std::exception_ptr meta_failure;
  try {
    co_await publishMeta(client_, bucket_, meta, digest);
  } catch (...) {
    meta_failure = std::current_exception();
  }
  if (meta_failure) {
    try {
      co_await purgeSubject(client_, bucket_, chunks_subject);
    } catch (...) {
    }
    std::rethrow_exception(meta_failure);
  }

  if (previous && !previous->deleted && !previous->nuid.empty()) {
    co_await purgeSubject(client_, bucket_, chunkSubject(bucket_, previous->nuid));
  }
  co_return ObjectInfo{std::move(name), meta.size, meta.chunks};
}

auto jetstream::ObjectStore::Store::info(std::string name) -> boost::cobalt::task<ObjectInfo> {
  if (name.empty()) {
    throw Error("object name is required");
  }
  const auto meta = co_await loadObject(client_, bucket_, name, false);
  if (!meta) {
    throw Error("object not found");
  }
  co_return ObjectInfo{std::move(name), meta->size, meta->chunks};
}

auto jetstream::ObjectStore::Store::get(std::string name) -> boost::cobalt::task<std::string> {
  if (name.empty()) {
    throw Error("object name is required");
  }
  const auto meta = co_await loadObject(client_, bucket_, name, false);
  if (!meta) {
    throw Error("object not found");
  }
  if (meta->chunks == 0 || meta->size == 0) {
    co_return std::string{};
  }

  const auto stream = "OBJ_" + bucket_;
  const auto subject = chunkSubject(bucket_, meta->nuid);
  auto current = co_await getMessage(client_, stream, std::string("{\"next_by_subj\":\"") + jsonEscape(subject) + "\"}");
  if (!current) {
    throw Error("object chunks not found");
  }

  std::string data = std::move(current->data);
  auto sequence = current->sequence;
  std::uint64_t found = 1;
  int misses = 0;
  while (found < meta->chunks) {
    ++sequence;
    if (++misses > 64) {
      throw Error("object chunks are incomplete");
    }
    auto next = co_await getMessage(client_, stream, std::string("{\"seq\":") + std::to_string(sequence) + "}");
    if (!next) {
      continue;
    }
    misses = 0;
    if (next->subject == subject) {
      data += next->data;
      ++found;
    }
  }
  co_return data;
}

auto jetstream::ObjectStore::Store::remove(std::string name) -> boost::cobalt::task<void> {
  if (name.empty()) {
    throw Error("object name is required");
  }
  const auto meta = co_await loadObject(client_, bucket_, name, true);
  if (!meta) {
    throw Error("object not found");
  }
  auto marker = *meta;
  marker.deleted = true;
  marker.size = 0;
  marker.chunks = 0;
  co_await publishMeta(client_, bucket_, marker, "");
  if (!meta->nuid.empty()) {
    co_await purgeSubject(client_, bucket_, chunkSubject(bucket_, meta->nuid));
  }
  co_return;
}

jetstream::Context::Context(Client client) : client_(std::move(client)) {}

auto jetstream::Context::createKeyValue(jetstream::kv::Config config) -> boost::cobalt::task<jetstream::kv::Store> {
  requireBucket(config.bucket);
  auto history = config.history < 1 ? std::int64_t{1} : config.history;
  if (history > 64) {
    throw Error("history must be between 1 and 64");
  }
  const auto body = std::string("{\"name\":\"KV_") + config.bucket + "\",\"subjects\":[\"$KV." + config.bucket +
                    ".>\"],\"retention\":\"limits\",\"max_consumers\":-1,\"max_msgs\":-1,\"max_bytes\":-1,"
                    "\"discard\":\"old\",\"max_age\":0,\"max_msgs_per_subject\":" +
                    std::to_string(history) +
                    ",\"max_msg_size\":-1,\"storage\":\"file\",\"num_replicas\":1,"
                    "\"duplicate_window\":120000000000,\"allow_rollup_hdrs\":true,\"deny_delete\":true,"
                    "\"allow_direct\":true}";
  const auto response = co_await call(client_, "$JS.API.STREAM.CREATE.KV_" + config.bucket, body);
  const auto failure = readApiFailure(response);
  if (failure.failed) {
    throw Error(failure.description);
  }
  co_return jetstream::kv::Store{client_, std::move(config.bucket)};
}

auto jetstream::Context::getKeyValue(std::string bucket) -> boost::cobalt::task<jetstream::kv::Store> {
  requireBucket(bucket);
  const auto response = co_await call(client_, "$JS.API.STREAM.INFO.KV_" + bucket, "{}");
  const auto failure = readApiFailure(response);
  if (failure.failed) {
    if (missingMessage(failure)) {
      throw Error("bucket not found");
    }
    throw Error(failure.description);
  }
  co_return jetstream::kv::Store{client_, std::move(bucket)};
}

auto jetstream::Context::createObjectStore(jetstream::ObjectStore::Config config) -> boost::cobalt::task<jetstream::ObjectStore::Store> {
  requireBucket(config.bucket);
  const auto body = std::string("{\"name\":\"OBJ_") + config.bucket + "\",\"subjects\":[\"$O." + config.bucket +
                    ".C.>\",\"$O." + config.bucket +
                    ".M.>\"],\"retention\":\"limits\",\"max_consumers\":-1,\"max_msgs\":-1,\"max_bytes\":-1,"
                    "\"discard\":\"new\",\"max_age\":0,\"storage\":\"file\",\"num_replicas\":1,"
                    "\"allow_rollup_hdrs\":true,\"allow_direct\":true}";
  const auto response = co_await call(client_, "$JS.API.STREAM.CREATE.OBJ_" + config.bucket, body);
  const auto failure = readApiFailure(response);
  if (failure.failed) {
    throw Error(failure.description);
  }
  co_return jetstream::ObjectStore::Store{client_, std::move(config.bucket)};
}

auto jetstream::Context::getObjectStore(std::string bucket) -> boost::cobalt::task<jetstream::ObjectStore::Store> {
  requireBucket(bucket);
  const auto response = co_await call(client_, "$JS.API.STREAM.INFO.OBJ_" + bucket, "{}");
  const auto failure = readApiFailure(response);
  if (failure.failed) {
    if (missingMessage(failure)) {
      throw Error("bucket not found");
    }
    throw Error(failure.description);
  }
  co_return jetstream::ObjectStore::Store{client_, std::move(bucket)};
}

auto jetstream::make(Client client) -> boost::cobalt::task<Context> {
  co_return Context{std::move(client)};
}

}  // namespace AsyncNats
