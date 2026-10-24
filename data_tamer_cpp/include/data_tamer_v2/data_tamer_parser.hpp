#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace DataTamerParser
{
inline namespace v2
{

/// Version of the schema, in the line format and in YAML (docs/wire_format.md).
constexpr int SCHEMA_VERSION = 5;

enum class BasicType : uint8_t
{
  BOOL,
  CHAR,
  INT8,
  UINT8,

  INT16,
  UINT16,

  INT32,
  UINT32,

  INT64,
  UINT64,

  FLOAT32,
  FLOAT64,
  OTHER,
  STRING  // text: a uint32 byte count, then the bytes
};

constexpr size_t TypesCount = 14;

namespace detail
{
/// A custom type section of the line format starts with a line of '=' (section 2 of
/// docs/wire_format.md): the writer emits kSeparatorWidth of them, the reader accepts
/// kMinSeparatorWidth or more.
constexpr size_t kSeparatorWidth = 59;
constexpr size_t kMinSeparatorWidth = 30;
}  // namespace detail

using VarNumber = std::variant<bool, char, int8_t, uint8_t, int16_t, uint16_t, int32_t,
                               uint32_t, int64_t, uint64_t, float, double>;

/// Non-owning view of bytes: the owner must keep them alive while the view is in use.
struct BufferSpan
{
  const uint8_t* data = nullptr;
  size_t size = 0;

  /// Drops the first `n` bytes; throws std::runtime_error if there are fewer.
  void trimFront(size_t n)
  {
    if(n > size)
    {
      throw std::runtime_error("DataTamerParser: payload truncated");
    }
    data += n;
    size -= n;
  }
};

VarNumber DeserializeToVarNumber(BasicType type, BufferSpan& buffer);

//---------------------------------------------------------
/// A schema field. `is_vector` covers `T[]` (array_size 0, a uint32 count precedes the
/// elements) and `T[N]` (array_size N, no count). `type` is OTHER for a custom type.
struct TypeField
{
  std::string field_name;
  BasicType type = BasicType::OTHER;
  std::string type_name;
  bool is_vector = false;
  uint32_t array_size = 0;

  bool operator==(const TypeField& other) const;
  bool operator!=(const TypeField& other) const;
};

using FieldsVector = std::vector<TypeField>;

/// A custom type whose layout is described in a foreign schema language.
struct CustomSchema
{
  std::string encoding;
  std::string schema;
};

/**
 * @brief A parsed schema (docs/wire_format.md, section 2). `fields[i]` is bit i of the
 * active mask.
 */
struct Schema
{
  uint64_t hash = 0;
  FieldsVector fields;
  std::string channel_name;

  std::map<std::string, FieldsVector> custom_types;
  /// Opaque custom types (an "ENCODING:" section, or `opaque_types` in YAML).
  std::map<std::string, CustomSchema> custom_schemas;
};

struct SnapshotView
{
  /// Hash of the schema that decodes this snapshot.
  uint64_t schema_hash = 0;

  /// Timestamp in nanoseconds, unused by the parser.
  uint64_t timestamp = 0;

  /// One bit per top-level field, least significant bit first (docs/wire_format.md,
  /// section 3.1).
  BufferSpan active_mask;

  /// Values of the active fields, in schema order (docs/wire_format.md, section 3.2).
  BufferSpan payload;
};

[[nodiscard]] bool GetBit(BufferSpan mask, size_t index);

/// Splits the body of an MCAP message (docs/wire_format.md, section 4.1) into the active
/// mask and the payload of a SnapshotView, whose schema hash and timestamp stay 0 for
/// the caller to fill in. Throws std::runtime_error if the body is shorter than the
/// lengths it declares or holds bytes after the payload.
[[nodiscard]] SnapshotView SplitMcapMessage(BufferSpan body);

/// Decodes `snapshot` with `schema`, calling
///   callback_number(const std::string& name, const VarNumber& value)
/// for each numeric value of each active field, in schema order. "string" values (text)
/// are skipped: pass a callback_string to the overload below to get them. Names join
/// nested fields with '/' and index container elements: "pose/position/x", "points[1]/z". `name` is valid
/// only during the call: copy it to keep it.
/// Returns false if the hash is not the schema's (nothing is decoded) or if bytes are
/// left after the last field (the callback has already run). Throws std::runtime_error
/// on malformed data, and for an active field of a type the schema does not define or
/// describes as opaque, but an empty vector of an opaque type decodes as empty.
template <typename NumberCallback>
[[nodiscard]] bool ParseSnapshot(const Schema& schema, SnapshotView snapshot,
                                 const NumberCallback& callback_number);

/// ParseSnapshot() that also decodes text: it calls
///   callback_string(const std::string& name, std::string_view text)
/// for each "string" value (a std::string or std::string_view in the writer), also an
/// empty one, in schema order with the numbers: "label", "tags[0]", "msg/label". `text`
/// is the bytes as written: no terminator, and not checked to be UTF-8. It points into
/// the payload, so it is valid as long as the payload: copy it to keep it longer.
/// "char" values, also of a "char[]" (std::vector<char>), go to callback_number.
template <typename NumberCallback, typename StringCallback>
[[nodiscard]] bool ParseSnapshot(const Schema& schema, SnapshotView snapshot,
                                 const NumberCallback& callback_number,
                                 const StringCallback& callback_string);

//---------------------------------------------------------
// Helpers for the data_tamer_msgs messages of ROS2PublisherSink. They are templates on
// the message type, so this header needs no ROS: any type with the same fields works.

/**
 * @brief Schemas by hash, filled from the `<prefix>/schemas` topic and/or from the
 * schemas embedded in batches.
 */
class SchemaRegistry
{
public:
  /// Parses and stores `schema_text` under `hash`, unless `hash` is already known.
  /// Throws std::runtime_error if the text is malformed or declares another hash.
  const Schema& add(uint64_t hash, const std::string& schema_text);

  /// Adds every entry of `msg.schemas` (each with `hash` and `schema_text`):
  /// a data_tamer_msgs Schemas or SnapshotBatch message.
  template <typename SchemasMsgT>
  void addSchemas(const SchemasMsgT& msg);

  /// nullptr if the hash is unknown.
  [[nodiscard]] const Schema* find(uint64_t hash) const;

  [[nodiscard]] size_t size() const { return schemas_.size(); }

private:
  std::unordered_map<uint64_t, Schema> schemas_;
};

/// View on a data_tamer_msgs Snapshot (`timestamp_nsec`, `schema_hash`,
/// `active_mask`, `payload`). The message must outlive the view.
template <typename SnapshotMsgT>
SnapshotView ToSnapshotView(const SnapshotMsgT& msg);

/**
 * @brief Calls `callback(const Schema&, const SnapshotView&)` for each snapshot of a
 * data_tamer_msgs SnapshotBatch, in order.
 * The schemas embedded in the batch are added to `registry` first; a malformed one
 * throws (see SchemaRegistry::add) before any snapshot is visited. Snapshots whose
 * schema is not in the registry are skipped.
 *
 * @return the number of snapshots visited.
 */
template <typename BatchMsgT, typename SnapshotCallback>
size_t ForEachSnapshotInBatch(SchemaRegistry& registry, const BatchMsgT& batch,
                              const SnapshotCallback& callback);

//---------------------------------------------------------
//---------------------------------------------------------
//---------------------------------------------------------

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "needs a little endian host");
#endif

/// Reads a T from the front of `buffer` and advances it; throws std::runtime_error if
/// it is too short. Copies the bytes as they are, so the host must be little endian.
template <typename T>
inline T Deserialize(BufferSpan& buffer)
{
  T var;
  const auto N = sizeof(T);
  if(N > buffer.size)
  {
    throw std::runtime_error("DataTamerParser: payload truncated");
  }
  if constexpr(std::is_same_v<T, bool>)
  {
    var = buffer.data[0] != 0;  // another byte value is not a valid bool
  }
  else
  {
    std::memcpy(&var, buffer.data, N);
  }
  buffer.data += N;
  buffer.size -= N;
  return var;
}

[[nodiscard]] inline SnapshotView SplitMcapMessage(BufferSpan body)
{
  SnapshotView view{};
  const uint32_t mask_size = Deserialize<uint32_t>(body);
  view.active_mask.data = body.data;
  body.trimFront(mask_size);
  view.active_mask.size = mask_size;
  const uint32_t payload_size = Deserialize<uint32_t>(body);
  view.payload.data = body.data;
  body.trimFront(payload_size);
  view.payload.size = payload_size;
  if(body.size != 0)
  {
    throw std::runtime_error("DataTamerParser: MCAP message body has trailing bytes");
  }
  return view;
}

inline VarNumber DeserializeToVarNumber(BasicType type, BufferSpan& buffer)
{
  switch(type)
  {
    case BasicType::BOOL:
      return Deserialize<bool>(buffer);
    case BasicType::CHAR:
      return Deserialize<char>(buffer);

    case BasicType::INT8:
      return Deserialize<int8_t>(buffer);
    case BasicType::UINT8:
      return Deserialize<uint8_t>(buffer);

    case BasicType::INT16:
      return Deserialize<int16_t>(buffer);
    case BasicType::UINT16:
      return Deserialize<uint16_t>(buffer);

    case BasicType::INT32:
      return Deserialize<int32_t>(buffer);
    case BasicType::UINT32:
      return Deserialize<uint32_t>(buffer);

    case BasicType::INT64:
      return Deserialize<int64_t>(buffer);
    case BasicType::UINT64:
      return Deserialize<uint64_t>(buffer);

    case BasicType::FLOAT32:
      return Deserialize<float>(buffer);
    case BasicType::FLOAT64:
      return Deserialize<double>(buffer);

    case BasicType::OTHER:
    case BasicType::STRING:
      return double(std::numeric_limits<double>::quiet_NaN());
  }
  return {};
}

[[nodiscard]] inline bool GetBit(BufferSpan mask, size_t index)
{
  if((index >> 3) >= mask.size)
  {
    throw std::runtime_error("DataTamerParser: active mask shorter than the schema");
  }
  const uint8_t& byte = mask.data[index >> 3];
  return 0 != (byte & uint8_t(1 << (index % 8)));
}

/// Folds `field` into a running schema hash with the version 4 recipe (data_tamer 1.x).
/// It is std::hash based, so it reproduces the writer's hash only with the same standard
/// library and word size (docs/wire_format.md, section 5).
[[nodiscard]] inline uint64_t AddFieldToHash(const TypeField& field, uint64_t hash)
{
  // https://stackoverflow.com/questions/2590677/how-do-i-combine-hash-values-in-c0x
  const std::hash<std::string> str_hasher;
  const std::hash<uint8_t> type_hasher;
  const std::hash<bool> bool_hasher;
  const std::hash<uint32_t> uint_hasher;

  auto combine = [&hash](const auto& hasher, const auto& val) {
    hash ^= hasher(val) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  };

  combine(str_hasher, field.field_name);
  combine(type_hasher, static_cast<uint8_t>(field.type));
  if(field.type == BasicType::OTHER)
  {
    combine(str_hasher, field.type_name);
  }
  combine(bool_hasher, field.is_vector);
  combine(uint_hasher, field.array_size);
  return hash;
}

/// Hash recipe of schema version 5: FNV-1a 64 of the schema text without its
/// "### hash:" line (docs/wire_format.md, section 5). Platform independent.
[[nodiscard]] inline uint64_t SchemaTextHash(const std::string& text)
{
  uint64_t hash = 0xcbf29ce484222325ULL;
  auto feed = [&hash](const char* begin, const char* end) {
    for(; begin != end; ++begin)
    {
      hash ^= static_cast<uint8_t>(*begin);
      hash *= 0x100000001b3ULL;
    }
  };
  const auto hash_line = text.find("### hash:");
  if(hash_line == std::string::npos)
  {
    feed(text.data(), text.data() + text.size());
    return hash;
  }
  const auto line_end = text.find('\n', hash_line);
  feed(text.data(), text.data() + hash_line);
  if(line_end != std::string::npos)
  {
    feed(text.data() + line_end + 1, text.data() + text.size());
  }
  return hash;
}

inline bool TypeField::operator==(const TypeField& other) const
{
  return is_vector == other.is_vector && type == other.type &&
         array_size == other.array_size && field_name == other.field_name &&
         type_name == other.type_name;
}

inline bool TypeField::operator!=(const TypeField& other) const
{
  return !(*this == other);
}

/// Renders a schema in the version 5 line format, byte for byte as the
/// DataTamer writer does: SchemaTextHash() of the result is the schema hash.
[[nodiscard]] inline std::string ToText(const Schema& schema)
{
  auto fieldLine = [](const TypeField& field) {
    std::string line = field.type_name;
    if(field.is_vector)
    {
      line += field.array_size == 0 ? "[]" : "[" + std::to_string(field.array_size) + "]";
    }
    return line + " " + field.field_name + "\n";
  };
  const std::string separator(detail::kSeparatorWidth, '=');
  std::string out = "### version: " + std::to_string(SCHEMA_VERSION) +
                    "\n### hash: " + std::to_string(schema.hash) +
                    "\n### channel_name: " + schema.channel_name + "\n\n";
  for(const auto& field : schema.fields)
  {
    out += fieldLine(field);
  }
  for(const auto& [type_name, fields] : schema.custom_types)
  {
    out += separator + "\nMSG: " + type_name + "\n";
    for(const auto& field : fields)
    {
      out += fieldLine(field);
    }
  }
  for(const auto& [type_name, custom] : schema.custom_schemas)
  {
    out += separator + "\nMSG: " + type_name + "\nENCODING: " + custom.encoding + "\n" +
           custom.schema + "\n";
  }
  return out;
}

namespace detail
{
/// Deeper YAML nesting is rejected (the writer nests at most 16 levels).
constexpr size_t kMaxYamlDepth = 64;

struct YamlNode
{
  std::string key;
  std::optional<std::string> scalar;  // empty for a mapping
  std::vector<YamlNode> children;
};

[[noreturn]] inline void YamlError(const std::string& what, const std::string& line)
{
  throw std::runtime_error("DataTamerParser: YAML schema: " + what + " in: " + line);
}

inline void AppendUtf8(std::string& out, uint32_t cp)
{
  if(cp < 0x80)
  {
    out += char(cp);
  }
  else if(cp < 0x800)
  {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  }
  else if(cp < 0x10000)
  {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
  else
  {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

/// Decodes the double-quoted scalar at s[pos] == '"'; returns the index past its end.
inline size_t ReadQuoted(const std::string& s, size_t pos, std::string& out,
                         const std::string& line)
{
  out.clear();
  size_t i = pos + 1;
  while(i < s.size())
  {
    const char c = s[i];
    if(c == '"')
    {
      return i + 1;
    }
    if(c != '\\')
    {
      out += c;
      i++;
      continue;
    }
    if(i + 1 >= s.size())
    {
      break;
    }
    const char e = s[i + 1];
    size_t digits = 0;
    switch(e)
    {
      case '"':
      case '\\':
      case '/':
        out += e;
        break;
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'r':
        out += '\r';
        break;
      case '0':
        out += '\0';
        break;
      case 'x':
        digits = 2;
        break;
      case 'u':
        digits = 4;
        break;
      case 'U':
        digits = 8;
        break;
      default:
        YamlError("bad escape", line);
    }
    i += 2;
    if(digits > 0)
    {
      const std::string hex = s.substr(i, digits);
      if(hex.size() != digits ||
         hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
      {
        YamlError("bad escape", line);
      }
      const auto cp = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
      if(cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      {
        YamlError("bad escape", line);
      }
      AppendUtf8(out, cp);
      i += digits;
    }
  }
  YamlError("unterminated string", line);
}

inline std::string YamlScalar(const std::string& s, const std::string& line)
{
  if(!s.empty() && s.front() == '"')
  {
    std::string value;
    const size_t end = ReadQuoted(s, 0, value, line);
    if(s.find_first_not_of(' ', end) != std::string::npos)
    {
      YamlError("unexpected text after string", line);
    }
    return value;
  }
  if(s.empty() || std::string("'[{&*!|>-?").find(s.front()) != std::string::npos ||
     s.find(" #") != std::string::npos)
  {
    YamlError("unsupported YAML", line);
  }
  return s;
}

/// Splits "key: value" or "key:" into the key and the trimmed value text.
inline std::pair<std::string, std::string> SplitYamlEntry(const std::string& content,
                                                          const std::string& line)
{
  std::string key;
  std::string rest;
  if(content.front() == '"')
  {
    const size_t end = ReadQuoted(content, 0, key, line);
    if(end >= content.size() || content[end] != ':')
    {
      YamlError("expected ':' after key", line);
    }
    rest = content.substr(end + 1);
  }
  else
  {
    size_t colon = content.find(": ");
    if(colon == std::string::npos)
    {
      if(content.back() != ':')
      {
        YamlError("expected 'key: value'", line);
      }
      colon = content.size() - 1;
    }
    key = YamlScalar(content.substr(0, colon), line);
    rest = content.substr(colon + 1);
  }
  if(!rest.empty() && rest.front() != ' ')
  {
    YamlError("expected a space after ':'", line);
  }
  const auto first = rest.find_first_not_of(' ');
  rest = first == std::string::npos ? std::string() : rest.substr(first);
  return { key, rest };
}

/// Type names of the schema text, indexed by BasicType.
inline const std::array<std::string, TypesCount>& BasicTypeNames()
{
  static const std::array<std::string, TypesCount> kNames = {
    "bool",   "char",  "int8",   "uint8",   "int16",   "uint16", "int32",
    "uint32", "int64", "uint64", "float32", "float64", "other",  "string"
  };
  return kNames;
}

/// BasicType named exactly `name`; OTHER for anything else (custom types).
inline BasicType BasicTypeFromName(const std::string& name)
{
  const auto& names = BasicTypeNames();
  for(size_t i = 0; i < TypesCount; i++)
  {
    if(i != size_t(BasicType::OTHER) && name == names[i])
    {
      return static_cast<BasicType>(i);
    }
  }
  return BasicType::OTHER;
}

/// Unsigned decimal, digits only. nullopt if invalid or above 2^64 - 1.
inline std::optional<uint64_t> ParseUnsigned(const std::string& value)
{
  if(value.empty() || value.size() > 20 ||
     value.find_first_not_of("0123456789") != std::string::npos)
  {
    return std::nullopt;
  }
  try
  {
    return std::stoull(value);
  }
  catch(const std::out_of_range&)
  {
    return std::nullopt;
  }
}

/// The N of a fixed-size array "[N]" (digits only). nullopt unless it is in 1..65535.
inline std::optional<uint16_t> ParseArrayExtent(const std::string& digits)
{
  const auto extent = ParseUnsigned(digits);
  if(!extent || *extent == 0 || *extent > 65535)
  {
    return std::nullopt;
  }
  return static_cast<uint16_t>(*extent);
}

/// Parses the block-mapping subset of YAML written by DataTamer2::ToYaml().
inline std::vector<YamlNode> ParseYamlTree(const std::string& txt)
{
  struct Level
  {
    int parent_indent;
    std::optional<int> indent;  // indentation of this mapping's entries
    std::vector<YamlNode>* entries;
    std::unordered_set<std::string> keys;
  };
  std::vector<YamlNode> root;
  std::vector<Level> stack = { { -1, 0, &root, {} } };  // top level starts at column 0

  std::istringstream ss(txt);
  std::string line;
  while(std::getline(ss, line))
  {
    while(!line.empty() && (line.back() == ' ' || line.back() == '\r'))
    {
      line.pop_back();
    }
    const size_t first = line.find_first_not_of(' ');
    if(first == std::string::npos || line[first] == '#')
    {
      continue;
    }
    if(line[first] == '\t')
    {
      YamlError("tab indentation", line);
    }
    const int indent = int(first);
    while(indent <= stack.back().parent_indent)
    {
      stack.pop_back();
    }
    Level& level = stack.back();
    if(!level.indent)
    {
      level.indent = indent;
    }
    else if(*level.indent != indent)
    {
      YamlError("bad indentation", line);
    }
    auto [key, rest] = SplitYamlEntry(line.substr(first), line);
    if(!level.keys.insert(key).second)
    {
      YamlError("duplicate key", line);
    }
    YamlNode node;
    node.key = std::move(key);
    if(!rest.empty() && rest != "{}")
    {
      node.scalar = YamlScalar(rest, line);
    }
    level.entries->push_back(std::move(node));
    if(rest.empty())
    {
      if(stack.size() > kMaxYamlDepth)
      {
        YamlError("nesting too deep", line);
      }
      stack.push_back({ indent, std::nullopt, &level.entries->back().children, {} });
    }
  }
  return root;
}

inline TypeField ParseYamlTypeSpec(const std::string& spec)
{
  TypeField field;
  const auto bracket = spec.find('[');
  field.type_name = spec.substr(0, bracket);
  if(field.type_name.empty())
  {
    throw std::runtime_error("DataTamerParser: YAML schema: empty type");
  }
  field.type = BasicTypeFromName(field.type_name);
  if(bracket != std::string::npos)
  {
    field.is_vector = true;
    const std::string inner = spec.substr(bracket + 1, spec.size() - bracket - 2);
    if(spec.back() != ']' || inner.find_first_not_of("0123456789") != std::string::npos)
    {
      throw std::runtime_error("DataTamerParser: YAML schema: invalid type " + spec);
    }
    if(!inner.empty())
    {
      const auto extent = ParseArrayExtent(inner);
      if(!extent)
      {
        throw std::runtime_error("DataTamerParser: YAML schema: array size out of "
                                 "range (1..65535) in " +
                                 spec);
      }
      field.array_size = *extent;
    }
  }
  return field;
}

/// True if `name` can name a field (docs/wire_format.md section 2): not empty, and no
/// byte up to the space or 0x7f. Bytes above 0x7f, as in UTF-8, are fine.
inline bool IsValidFieldName(std::string_view name)
{
  return !name.empty() && std::none_of(name.begin(), name.end(), [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte <= 0x20 || byte == 0x7f;
  });
}

inline void FlattenYamlFields(const std::vector<YamlNode>& nodes,
                              const std::string& prefix, FieldsVector& out)
{
  for(const auto& node : nodes)
  {
    if(node.scalar)
    {
      TypeField field = ParseYamlTypeSpec(*node.scalar);
      field.field_name = prefix + node.key;
      if(!IsValidFieldName(field.field_name))
      {
        throw std::runtime_error("DataTamerParser: YAML schema: invalid field name " +
                                 field.field_name);
      }
      out.push_back(std::move(field));
    }
    else
    {
      FlattenYamlFields(node.children, prefix + node.key + "/", out);
    }
  }
}

inline const YamlNode* FindYamlKey(const std::vector<YamlNode>& nodes,
                                   const std::string& key)
{
  for(const auto& node : nodes)
  {
    if(node.key == key)
    {
      return &node;
    }
  }
  return nullptr;
}

/// Removes the spaces and carriage returns at both ends of `str`.
inline void Trim(std::string& str)
{
  const auto last = str.find_last_not_of(" \r");
  if(last == std::string::npos)
  {
    str.clear();
    return;
  }
  str.erase(last + 1);
  str.erase(0, str.find_first_not_of(" \r"));
}

/// Reads the line of `txt` that starts at `offset` into `line`, without its '\n', and
/// moves `offset` to the start of the next one. Returns false at the end of the text.
inline bool ReadLine(const std::string& txt, size_t& offset, std::string& line)
{
  if(offset >= txt.size())
  {
    return false;
  }
  const size_t end = std::min(txt.find('\n', offset), txt.size());
  line.assign(txt, offset, end - offset);
  offset = std::min(end + 1, txt.size());
  return true;
}

/// Reads the next line of `txt` that is not blank, trimmed. Returns false, with `line`
/// empty, if there is none.
inline bool ReadNonBlankLine(const std::string& txt, size_t& offset, std::string& line)
{
  while(ReadLine(txt, offset, line))
  {
    Trim(line);
    if(!line.empty())
    {
      return true;
    }
  }
  line.clear();
  return false;
}

/// True for the YAML rendering (version 5): its first line that is not a comment starts
/// with "version:", where the line format starts with a "###" header.
inline bool IsYamlSchema(const std::string& txt)
{
  size_t offset = 0;
  std::string line;
  while(ReadNonBlankLine(txt, offset, line))
  {
    if(line.front() != '#' || line.rfind("###", 0) == 0)
    {
      return line.rfind("version:", 0) == 0;
    }
  }
  return false;
}

/// If `line` starts with `prefix` (such as "### hash:"), puts the rest of it, trimmed, in
/// `value`.
inline bool ReadHeaderValue(const std::string& line, const char* prefix,
                            std::string& value)
{
  const size_t size = std::strlen(prefix);
  if(line.compare(0, size, prefix) != 0)
  {
    return false;
  }
  value.assign(line, size, std::string::npos);
  Trim(value);
  return true;
}

/// The array size of the type spec `spec`, whose '[' is at `open` and whose ']' ends it:
/// 0 for "[]" (a dynamic vector), N for "[N]". `line` is the field line, for the error
/// messages.
inline uint32_t ParseArraySize(const std::string& spec, size_t open,
                               const std::string& line)
{
  const auto close = spec.find(']', open);
  if(close == std::string::npos)
  {
    throw std::runtime_error("Unterminated array size in: " + line);
  }
  if(close + 1 != spec.size())
  {
    throw std::runtime_error("Unexpected text after the array size in: " + line);
  }
  if(close == open + 1)
  {
    return 0;
  }
  const std::string digits = spec.substr(open + 1, close - open - 1);
  if(digits.find_first_not_of("0123456789") != std::string::npos)
  {
    throw std::runtime_error("Invalid array size in: " + line);
  }
  const auto extent = ParseArrayExtent(digits);
  if(!extent)
  {
    throw std::runtime_error("Array size out of range (1..65535) in: " + line);
  }
  return *extent;
}

/// Parses a field line, "<type> <name>", the type being "T", "T[]" or "T[N]".
inline TypeField ParseFieldLine(const std::string& line)
{
  // Split at the first space.
  const auto space_pos = line.find(' ');
  if(space_pos == std::string::npos)
  {
    throw std::runtime_error("Unexpected line: " + line);
  }
  std::string type_spec = line.substr(0, space_pos);
  std::string name = line.substr(space_pos + 1);
  Trim(type_spec);
  Trim(name);

  const auto open = type_spec.find('[');
  if(type_spec.empty() || open == 0)
  {
    throw std::runtime_error("Empty type name in: " + line);
  }
  TypeField field;
  // A type name is the whole token before any "[...]": "float64Pose" is a custom type.
  field.type_name = type_spec.substr(0, open);
  field.type = BasicTypeFromName(field.type_name);
  if(open != std::string::npos)
  {
    field.is_vector = true;
    field.array_size = ParseArraySize(type_spec, open, line);
  }
  if(!IsValidFieldName(name))
  {
    throw std::runtime_error("Invalid field name in: " + line);
  }
  field.field_name = std::move(name);
  return field;
}

/// The error for a line format text without a version line before its fields, such as
/// the texts of data_tamer releases before 0.7.0 (docs/wire_format.md, section 2).
inline std::runtime_error MissingVersionError()
{
  return std::runtime_error("DataTamerParser: no \"### version:\" line before the "
                            "fields; the supported schema versions are 4 and 5, so "
                            "texts written before data_tamer 0.7.0 are not readable");
}
}  // namespace detail

/// Parses a YAML schema (version 5, docs/wire_format.md section 2.1).
/// BuildSchemaFromText() calls it when the text starts with "version:".
[[nodiscard]] inline Schema BuildSchemaFromYaml(const std::string& txt,
                                                bool check_hash = false)
{
  using detail::FindYamlKey;
  const auto root = detail::ParseYamlTree(txt);
  auto scalar = [&](const std::string& key) -> const std::string& {
    const auto* node = FindYamlKey(root, key);
    if(!node || !node->scalar)
    {
      throw std::runtime_error("DataTamerParser: YAML schema: missing " + key);
    }
    return *node->scalar;
  };
  auto mapping = [&](const std::string& key, bool required) {
    const auto* node = FindYamlKey(root, key);
    if(node && node->scalar)
    {
      throw std::runtime_error("DataTamerParser: YAML schema: " + key +
                               " must be a mapping");
    }
    if(!node && required)
    {
      throw std::runtime_error("DataTamerParser: YAML schema: missing " + key);
    }
    return node ? &node->children : nullptr;
  };
  auto toUint = [](const std::string& value, const char* what) {
    const auto parsed = detail::ParseUnsigned(value);
    if(!parsed)
    {
      throw std::runtime_error(std::string("DataTamerParser: YAML schema: invalid ") +
                               what);
    }
    return *parsed;
  };

  if(toUint(scalar("version"), "version") != SCHEMA_VERSION)
  {
    throw std::runtime_error("Wrong SCHEMA_VERSION");
  }
  Schema schema;
  schema.hash = toUint(scalar("hash"), "hash");
  schema.channel_name = scalar("channel_name");
  detail::FlattenYamlFields(*mapping("fields", true), "", schema.fields);
  if(const auto* types = mapping("types", false))
  {
    for(const auto& type : *types)
    {
      if(type.scalar)
      {
        throw std::runtime_error("DataTamerParser: YAML schema: type " + type.key +
                                 " must be a mapping");
      }
      detail::FlattenYamlFields(type.children, "", schema.custom_types[type.key]);
    }
  }
  if(const auto* opaque = mapping("opaque_types", false))
  {
    for(const auto& type : *opaque)
    {
      const auto* encoding = FindYamlKey(type.children, "encoding");
      const auto* body = FindYamlKey(type.children, "schema");
      if(!encoding || !encoding->scalar || !body || !body->scalar)
      {
        throw std::runtime_error("DataTamerParser: YAML schema: opaque type " + type.key +
                                 " needs encoding and schema");
      }
      schema.custom_schemas[type.key] = { *encoding->scalar, *body->scalar };
    }
  }
  if(check_hash && schema.hash != SchemaTextHash(ToText(schema)))
  {
    throw std::runtime_error("Error in hash calculation");
  }
  return schema;
}

/// Parses a schema text in the line format (versions 4 and 5) or YAML (version 5), see
/// docs/wire_format.md section 2. Throws std::runtime_error on a malformed text or
/// another version; `check_hash` also throws if the declared hash is wrong. The declared
/// hash is the schema's hash. For version 4 (data_tamer 1.x) `check_hash` recomputes it
/// with this program's std::hash, which agrees with the writer's only for the same
/// standard library and word size: an intact 1.x text from another platform throws. The
/// Python decoder accepts a version 4 hash unchecked instead (wire_format.md, section 5).
[[nodiscard]] inline Schema BuildSchemaFromText(const std::string& txt,
                                                bool check_hash = false)
{
  if(detail::IsYamlSchema(txt))
  {
    return BuildSchemaFromYaml(txt, check_hash);
  }

  Schema schema;
  uint64_t declared_schema = 0;
  int version = 0;  // 0 until the version line

  std::vector<TypeField>* field_vector = &schema.fields;
  bool any_line = false;
  std::string section_name;     // the type of the last "MSG:" line
  bool section_starts = false;  // the line being read follows that "MSG:" line

  size_t offset = 0;  // where the next line of txt starts
  std::string line;
  while(detail::ReadNonBlankLine(txt, offset, line))
  {
    any_line = true;
    const bool opens_section = std::exchange(section_starts, false);
    if(line.size() >= detail::kMinSeparatorWidth &&
       line.find_first_not_of('=') == std::string::npos)
    {
      // a separator: the next line that is not blank is "MSG: <type name>"
      if(!detail::ReadNonBlankLine(txt, offset, line) || line.rfind("MSG: ", 0) != 0)
      {
        throw std::runtime_error("Expecting \"MSG: \" at the beginning of line: " + line);
      }
      line.erase(0, 5);
      detail::Trim(line);
      section_name = line;
      section_starts = true;
      field_vector = &schema.custom_types[section_name];
      continue;
    }
    if(line.rfind("MSG: ", 0) == 0)
    {
      throw std::runtime_error("\"MSG: \" not right after a separator, in: " + line);
    }

    if(line.rfind("ENCODING: ", 0) == 0)
    {
      // an opaque type: the foreign schema is the rest of the text, one final newline
      // (which the writer adds) apart
      if(!opens_section)
      {
        throw std::runtime_error("ENCODING: outside the start of a section, in: " + line);
      }
      std::string encoding = line.substr(10);
      detail::Trim(encoding);
      std::string foreign = txt.substr(offset);
      if(!foreign.empty() && foreign.back() == '\n')
      {
        foreign.pop_back();
      }
      schema.custom_types.erase(section_name);
      schema.custom_schemas[section_name] = { encoding, foreign };
      break;
    }

    // Headers: "### key: value", the value being what follows the colon. Only a line
    // that starts with "### " can be one, which spares the other lines the key tests.
    if(line.rfind("### ", 0) == 0)
    {
      std::string value;
      if(detail::ReadHeaderValue(line, "### version:", value))
      {
        const auto number = detail::ParseUnsigned(value);
        if(!number)
        {
          throw std::runtime_error("DataTamerParser: invalid version in: " + line);
        }
        // Version 4 differs only in how the hash was computed.
        if(*number != uint64_t(SCHEMA_VERSION) && *number != 4)
        {
          throw std::runtime_error("Wrong SCHEMA_VERSION");
        }
        version = int(*number);
        continue;
      }
      if(detail::ReadHeaderValue(line, "### hash:", value))
      {
        const auto number = detail::ParseUnsigned(value);
        if(!number)
        {
          throw std::runtime_error("DataTamerParser: invalid hash in: " + line);
        }
        declared_schema = *number;
        continue;
      }
      if(detail::ReadHeaderValue(line, "### channel_name:", value))
      {
        schema.channel_name = value;
        continue;
      }
    }

    if(version == 0)
    {
      throw detail::MissingVersionError();
    }
    field_vector->push_back(detail::ParseFieldLine(line));
  }
  if(!any_line)
  {
    throw std::runtime_error("DataTamerParser: empty schema text");
  }
  if(version == 0)
  {
    throw detail::MissingVersionError();
  }
  // The declared hash is the one snapshots carry. check_hash recomputes it with the
  // recipe of the text's version: for version 4, the channel name and the top-level
  // fields folded into a std::hash.
  uint64_t computed = SchemaTextHash(txt);
  if(version == 4)
  {
    computed = std::hash<std::string>()(schema.channel_name);
    for(const auto& field : schema.fields)
    {
      computed = AddFieldToHash(field, computed);
    }
  }
  if(check_hash && declared_schema != 0 && declared_schema != computed)
  {
    throw std::runtime_error("Error in hash calculation");
  }
  schema.hash = declared_schema != 0 ? declared_schema : computed;
  return schema;
}

/// Old misspelled name, kept for one release so that existing parsers compile.
[[deprecated("use BuildSchemaFromText")]] inline Schema
BuilSchemaFromText(const std::string& txt, bool check_hash = false)
{
  return BuildSchemaFromText(txt, check_hash);
}

namespace detail
{
/// Wire size in bytes of a basic type (0 for OTHER and STRING, whose size varies).
inline size_t SizeOf(BasicType type)
{
  switch(type)
  {
    case BasicType::BOOL:
    case BasicType::CHAR:
    case BasicType::INT8:
    case BasicType::UINT8:
      return 1;
    case BasicType::INT16:
    case BasicType::UINT16:
      return 2;
    case BasicType::INT32:
    case BasicType::UINT32:
    case BasicType::FLOAT32:
      return 4;
    case BasicType::INT64:
    case BasicType::UINT64:
    case BasicType::FLOAT64:
      return 8;
    default:
      return 0;
  }
}

/// Nested custom types deeper than this are treated as a malformed (cyclic) schema.
constexpr int kMaxSchemaDepth = 64;

/// Fewest payload bytes one value of a custom type takes, by type name, for the types a
/// decode has met so far. A schema has few custom types: the first ones are kept in the
/// object, so that a decode allocates nothing for them. The names are the schema's own
/// strings, which must outlive the object.
class MinSizes
{
public:
  struct Entry
  {
    size_t size;  // fewest bytes
    int height;   // how many custom types deep the type's fixed-size fields go
  };

  /// The entry stored for `type_name`, or nullptr.
  const Entry* find(const std::string& type_name) const
  {
    for(size_t i = 0; i < in_place_; i++)
    {
      if(*names_[i] == type_name)
      {
        return &entries_[i];
      }
    }
    const auto it = more_.find(type_name);
    return it == more_.end() ? nullptr : &it->second;
  }

  void insert(const std::string& type_name, Entry entry)
  {
    if(in_place_ < kInPlace)
    {
      names_[in_place_] = &type_name;
      entries_[in_place_++] = entry;
    }
    else
    {
      more_.emplace(type_name, entry);
    }
  }

private:
  static constexpr size_t kInPlace = 4;
  std::array<const std::string*, kInPlace> names_{};
  std::array<Entry, kInPlace> entries_{};
  size_t in_place_ = 0;
  std::map<std::string, Entry> more_;
};

/// Larger sizes saturate here, so that products of array extents cannot overflow.
constexpr size_t kHugeSize = std::numeric_limits<size_t>::max() / 2;

inline size_t MinFieldSize(const TypeField& field, const Schema& schema,
                           MinSizes& min_sizes, int depth);

/// The error of CheckDepth(), kept out of the line that tests the depth.
[[noreturn]] inline void NestedTooDeeply()
{
  throw std::runtime_error("DataTamerParser: custom types nested too deeply (cycle?)");
}

/// Throws if a field sits more than kMaxSchemaDepth custom types deep, which only a
/// malformed or cyclic schema does.
inline void CheckDepth(int depth)
{
  if(depth > kMaxSchemaDepth)
  {
    NestedTooDeeply();
  }
}

/// The error for a custom type the schema does not describe field by field.
[[noreturn]] inline void UndecodableType(const Schema& schema, const std::string& name)
{
  if(schema.custom_schemas.count(name) != 0)
  {
    throw std::runtime_error("DataTamerParser: type " + name +
                             " has an opaque encoding; cannot continue");
  }
  throw std::runtime_error("DataTamerParser: unknown type " + name);
}

/// The fields of the custom type `name`. Throws if the schema does not describe the type
/// field by field: it is opaque or not defined.
inline const FieldsVector& CustomFields(const Schema& schema, const std::string& name)
{
  const auto it = schema.custom_types.find(name);
  if(it == schema.custom_types.end())
  {
    UndecodableType(schema, name);
  }
  return it->second;
}

/// Fewest payload bytes one element of `field` takes: a basic type's size, or the sum
/// of the fewest bytes of each field of the custom type. A custom type found in
/// `min_sizes` throws as deep as computing it again would, so that the depth limit does
/// not depend on the fields decoded before.
inline size_t MinElementSize(const TypeField& field, const Schema& schema,
                             MinSizes& min_sizes, int depth)
{
  if(field.type == BasicType::STRING)
  {
    return sizeof(uint32_t);  // the byte count of an empty text
  }
  if(field.type != BasicType::OTHER)
  {
    return SizeOf(field.type);
  }
  if(const MinSizes::Entry* known = min_sizes.find(field.type_name))
  {
    CheckDepth(depth + known->height);
    return known->size;
  }
  CheckDepth(depth);
  size_t total = 0;
  int height = 0;
  for(const auto& sub_field : CustomFields(schema, field.type_name))
  {
    const size_t size = MinFieldSize(sub_field, schema, min_sizes, depth + 1);
    total = size > kHugeSize - total ? kHugeSize : total + size;
    // MinFieldSize() stored the type of a custom field, unless it is a dynamic vector
    if(sub_field.type == BasicType::OTHER &&
       !(sub_field.is_vector && sub_field.array_size == 0))
    {
      height = std::max(height, min_sizes.find(sub_field.type_name)->height + 1);
    }
  }
  min_sizes.insert(field.type_name, { total, height });
  return total;
}

/// Fewest payload bytes `field` takes: its elements at their fewest bytes, or only the
/// 4 byte count for a dynamic vector.
inline size_t MinFieldSize(const TypeField& field, const Schema& schema,
                           MinSizes& min_sizes, int depth)
{
  if(field.is_vector && field.array_size == 0)
  {
    return sizeof(uint32_t);
  }
  const size_t element = MinElementSize(field, schema, min_sizes, depth);
  const size_t count = field.is_vector ? field.array_size : 1;
  return (element != 0 && count > kHugeSize / element) ? kHugeSize : element * count;
}

/// Keeps the first `size` characters of `name` and writes the `length` characters of
/// `text` after them. `name` is resized only when its length changes, so that names of
/// one length overwrite their characters in place.
inline void SetTail(std::string& name, size_t size, const char* text, size_t length)
{
  if(name.size() != size + length)
  {
    name.resize(size + length);
  }
  std::memcpy(&name[size], text, length);
}

/// Makes `name` its first `parent_size` characters (the parent's name, none at the top
/// level), a '/' if there are any, and `field_name`.
inline void SetFieldName(std::string& name, size_t parent_size,
                         const std::string& field_name)
{
  if(parent_size == 0)
  {
    SetTail(name, 0, field_name.data(), field_name.size());
    return;
  }
  const size_t size = parent_size + 1 + field_name.size();
  if(name.size() != size)
  {
    name.resize(size);
  }
  name[parent_size] = '/';
  std::memcpy(&name[parent_size + 1], field_name.data(), field_name.size());
}

/// Makes the name of a vector element the name of the next one, in place: `name` holds
/// the field's name in its first `field_size` characters, then the element's index in
/// `index_size` characters ("[9]"), then whatever the element left. Returns the size of
/// the new index ("[10]").
inline size_t NextIndex(std::string& name, size_t field_size, size_t index_size)
{
  if(name.size() != field_size + index_size)
  {
    name.resize(field_size + index_size);
  }
  for(size_t i = name.size() - 2; i > field_size; i--)  // the digits, from the last one
  {
    if(name[i] != '9')
    {
      name[i]++;
      return index_size;
    }
    name[i] = '0';
  }
  // All nines: one more digit, "[99]" becomes "[100]".
  name[field_size + 1] = '1';
  name.back() = '0';
  name.push_back(']');
  return index_size + 1;
}

/// The fields of the custom types a decode has met below the top level, by the schema
/// field that names the type, so that the elements of a vector look the types of their
/// fields up once rather than once each. Past kInPlace fields it looks them up each time.
class FieldTypes
{
public:
  const FieldsVector& of(const TypeField& field, const Schema& schema)
  {
    for(size_t i = 0; i < count_; i++)
    {
      if(fields_[i] == &field)
      {
        return *types_[i];
      }
    }
    const FieldsVector& type = CustomFields(schema, field.type_name);
    if(count_ < kInPlace)
    {
      fields_[count_] = &field;
      types_[count_++] = &type;
    }
    return type;
  }

private:
  static constexpr size_t kInPlace = 16;
  // Not zeroed: a decode would pay for it, and only the first count_ entries are read.
  std::array<const TypeField*, kInPlace> fields_;
  std::array<const FieldsVector*, kInPlace> types_;
  size_t count_ = 0;
};

/// What one decode keeps from one value to the next.
struct DecodeState
{
  MinSizes min_sizes;
  FieldTypes types;
  std::string name;  // the name of the value being decoded
};

/// Reads one "string" value and passes it to callback_string.
template <typename StringCallback>
void DecodeText(BufferSpan& buffer, const std::string& name,
                const StringCallback& callback_string)
{
  const uint32_t size = Deserialize<uint32_t>(buffer);
  const auto* text = reinterpret_cast<const char*>(buffer.data);
  buffer.trimFront(size);
  callback_string(name, std::string_view(text, size));
}

/// Decodes `field`. The first `parent_size` characters of `state.name` are its parent's
/// name (none at the top level), the rest is scratch.
template <typename NumberCallback, typename StringCallback>
void ParseSnapshotRecursive(const TypeField& field, const Schema& schema,
                            BufferSpan& buffer, const NumberCallback& callback_number,
                            const StringCallback& callback_string, DecodeState& state,
                            size_t parent_size, int depth = 0)
{
  CheckDepth(depth);
  uint32_t vect_size = field.array_size;
  if(field.is_vector && field.array_size == 0)
  {
    vect_size = Deserialize<uint32_t>(buffer);
  }
  if(field.is_vector)
  {
    if(vect_size == 0)
    {
      // Nothing to read: an opaque type has nothing to skip, but a type the schema does
      // not define at all makes it malformed, whatever the count.
      if(field.type == BasicType::OTHER &&
         schema.custom_types.count(field.type_name) == 0 &&
         schema.custom_schemas.count(field.type_name) == 0)
      {
        UndecodableType(schema, field.type_name);
      }
      return;
    }
    // An element takes at least min_size bytes: a count the payload cannot hold is
    // rejected. Elements of no byte hold no value, however many there are.
    const size_t min_size = MinElementSize(field, schema, state.min_sizes, depth);
    if(min_size == 0)
    {
      return;
    }
    if(vect_size > buffer.size / min_size)
    {
      throw std::runtime_error("DataTamerParser: payload truncated");
    }
  }

  // A top-level field is decoded once: only the fields below it take a slot.
  const FieldsVector* const sub_fields =
      field.type != BasicType::OTHER ? nullptr :
      depth == 0                     ? &CustomFields(schema, field.type_name) :
                                       &state.types.of(field, schema);

  std::string& name = state.name;
  // `name` holds the name of one value, or of one element of a custom type.
  auto doParse = [&]() {
    if(field.type == BasicType::STRING)
    {
      DecodeText(buffer, std::as_const(name), callback_string);
    }
    else if(sub_fields == nullptr)
    {
      const auto var = DeserializeToVarNumber(field.type, buffer);
      callback_number(std::as_const(name), var);  // const: a callback cannot move it away
    }
    else
    {
      const size_t size = name.size();
      for(const auto& sub_field : *sub_fields)
      {
        ParseSnapshotRecursive(sub_field, schema, buffer, callback_number,
                               callback_string, state, size, depth + 1);
      }
    }
  };

  SetFieldName(name, parent_size, field.field_name);
  if(!field.is_vector)
  {
    doParse();
  }
  else
  {
    // The index follows the field's name: "[0]", then incremented in place.
    const size_t field_size = name.size();
    name.append("[0]");
    size_t index_size = 3;
    for(uint32_t a = 0; a < vect_size; a++)
    {
      if(a != 0)
      {
        index_size = NextIndex(name, field_size, index_size);
      }
      doParse();
    }
  }
}
}  // namespace detail

template <typename NumberCallback>
[[nodiscard]] inline bool ParseSnapshot(const Schema& schema, SnapshotView snapshot,
                                        const NumberCallback& callback_number)
{
  return ParseSnapshot(schema, snapshot, callback_number,
                       [](const std::string&, std::string_view) {});
}

template <typename NumberCallback, typename StringCallback>
[[nodiscard]] inline bool ParseSnapshot(const Schema& schema, SnapshotView snapshot,
                                        const NumberCallback& callback_number,
                                        const StringCallback& callback_string)
{
  if(schema.hash != snapshot.schema_hash)
  {
    return false;
  }
  BufferSpan buffer = snapshot.payload;
  detail::DecodeState state;
  if(snapshot.active_mask.size * 8 < schema.fields.size())
  {
    throw std::runtime_error("DataTamerParser: active mask shorter than the schema");
  }

  for(size_t i = 0; i < schema.fields.size(); i++)
  {
    const auto& field = schema.fields[i];
    if(!GetBit(snapshot.active_mask, i))
    {
      continue;
    }
    if(field.type == BasicType::STRING && !field.is_vector)
    {
      detail::DecodeText(buffer, field.field_name, callback_string);
    }
    else if(field.type != BasicType::OTHER && !field.is_vector)
    {
      // A top-level scalar is named as it is, without a copy.
      const auto var = DeserializeToVarNumber(field.type, buffer);
      callback_number(field.field_name, var);
    }
    else
    {
      detail::ParseSnapshotRecursive(field, schema, buffer, callback_number,
                                     callback_string, state, 0);
    }
  }
  // leftover bytes: the schema and the payload do not belong together
  return buffer.size == 0;
}

inline const Schema& SchemaRegistry::add(uint64_t hash, const std::string& schema_text)
{
  auto it = schemas_.find(hash);
  if(it == schemas_.end())
  {
    Schema schema = BuildSchemaFromText(schema_text);
    if(schema.hash != hash)
    {
      // snapshots carry `hash`: a schema declaring another one would never match them
      throw std::runtime_error("DataTamerParser: schema text declares hash " +
                               std::to_string(schema.hash) + ", expected " +
                               std::to_string(hash));
    }
    it = schemas_.emplace(hash, std::move(schema)).first;
  }
  return it->second;
}

template <typename SchemasMsgT>
inline void SchemaRegistry::addSchemas(const SchemasMsgT& msg)
{
  for(const auto& schema_msg : msg.schemas)
  {
    add(schema_msg.hash, schema_msg.schema_text);
  }
}

inline const Schema* SchemaRegistry::find(uint64_t hash) const
{
  auto it = schemas_.find(hash);
  return it == schemas_.end() ? nullptr : &it->second;
}

template <typename SnapshotMsgT>
inline SnapshotView ToSnapshotView(const SnapshotMsgT& msg)
{
  SnapshotView view;
  view.schema_hash = msg.schema_hash;
  view.timestamp = msg.timestamp_nsec;
  view.active_mask = { msg.active_mask.data(), msg.active_mask.size() };
  view.payload = { msg.payload.data(), msg.payload.size() };
  return view;
}

template <typename BatchMsgT, typename SnapshotCallback>
inline size_t ForEachSnapshotInBatch(SchemaRegistry& registry, const BatchMsgT& batch,
                                     const SnapshotCallback& callback)
{
  registry.addSchemas(batch);
  size_t visited = 0;
  for(const auto& snapshot_msg : batch.snapshots)
  {
    if(const Schema* schema = registry.find(snapshot_msg.schema_hash))
    {
      callback(*schema, ToSnapshotView(snapshot_msg));
      visited++;
    }
  }
  return visited;
}

}  // namespace v2
}  // namespace DataTamerParser
