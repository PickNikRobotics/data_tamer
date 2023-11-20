#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <optional>
#include <vector>
#include <variant>
#include <unordered_map>
#include <limits>
#include <functional>

#define DATA_TAMER_PROTOCOL "0.4"
#define DATA_TAMER_PROTOCOL_MAJOR 0
#define DATA_TAMER_PROTOCOL_MINOR 4

namespace DataTamerParser {

enum class BasicType
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
  OTHER
};

constexpr size_t TypesCount = 13;

using VarNumber = std::variant<
    bool, char,
    int8_t, uint8_t,
    int16_t, uint16_t,
    int32_t, uint32_t,
    int64_t, uint64_t,
    float, double >;

VarNumber DeserializeToVarNumber(BasicType type, const void* src);

/// Return the number of bytes needed to serialize the type
size_t SizeOf(const BasicType& type);

const std::string& ToStr(const BasicType& type);

/// Convert string to its type
BasicType FromStr(const std::string &str);

/**
 * @brief DataTamer uses a simple "flat" schema of key/value pairs (each pair is a "field").
 */
struct Schema
{
  struct Field
  {
    std::string name;
    BasicType type = BasicType::OTHER;
    bool is_vector = 0;
    uint16_t array_size = 0;

    bool operator==(const Field &other) const;

    bool operator!=(const Field &other) const {
      return !(*this == other);
    }
  };
  std::vector<Field> fields;
  uint64_t hash = 0;
};

std::optional<Schema> BuilSchemaFromText(const std::string& txt);

struct BufferSpan
{
  const uint8_t* data = nullptr;
  size_t size = 0;

  void trimFront(size_t n) {
    data += n;
    size -= n;
  }
};

struct SnapshotView {
  /// Unique identifier of the schema
  size_t schema_hash;

  /// snapshot timestamp
  uint64_t timestamp;

  /// Vector that tell us if a field of the schema is
  /// active or not. It is basically an optimized vector
  /// of bools, where each byte contains 8 boolean flags.
  BufferSpan active_mask;

  /// serialized data containing all the values, ordered as in the schema
  BufferSpan payload;
};

bool GetBit(BufferSpan mask, size_t index);

// Callback must be a std::function or lambda with signature:
//
// void(const std::string& name_field, const VarNumber& value)
//
template <typename Callback>
bool ParseSnapshot(const Schema& schema,
                   SnapshotView snapshot,
                   Callback& callback);

//---------------------------------------------------------
//---------------------------------------------------------
//---------------------------------------------------------

inline BasicType FromStr(const std::string& str)
{
  static const auto kMap = []() {
    std::unordered_map<std::string, BasicType> map;
    for(size_t i=0; i<TypesCount; i++)
    {
      auto type = static_cast<BasicType>(i);
      map[ToStr(type)] = type;
    }
    return map;
  }();

  auto const it = kMap.find(str);
  return it == kMap.end() ? BasicType::OTHER : it->second;
}

template<typename T> inline
    T Deserialize(BufferSpan& buffer)
{
  T var;
  const auto N = sizeof(T);
  std::memcpy(&var, buffer.data, N);
  buffer.data += N;
  if( N > buffer.size)
  {
    throw std::runtime_error("Buffer overflow");
  }
  buffer.size -= N;
  return var;
}

inline VarNumber DeserializeToVarNumber(BasicType type,
                                        BufferSpan& src)
{
  switch(type)
  {
    case BasicType::BOOL: return Deserialize<bool>(src);
    case BasicType::CHAR: return Deserialize<char>(src);

    case BasicType::INT8: return Deserialize<int8_t>(src);
    case BasicType::UINT8: return Deserialize<uint8_t>(src);

    case BasicType::INT16: return Deserialize<int16_t>(src);
    case BasicType::UINT16: return Deserialize<uint16_t>(src);

    case BasicType::INT32: return Deserialize<int32_t>(src);
    case BasicType::UINT32: return Deserialize<uint32_t>(src);

    case BasicType::INT64: return Deserialize<int64_t>(src);
    case BasicType::UINT64: return Deserialize<uint64_t>(src);

    case BasicType::FLOAT32: return Deserialize<float>(src);
    case BasicType::FLOAT64: return Deserialize<double>(src);

    case BasicType::OTHER:
      return double(std::numeric_limits<double>::quiet_NaN());
  }
  return {};
}

inline size_t SizeOf(const BasicType& type)
{
  static constexpr std::array<size_t, TypesCount> kSizes =
      { 1, 1,
       1, 1,
       2, 2, 4, 4, 8, 8,
       4, 8, 0 };
  return kSizes[static_cast<size_t>(type)];
}

const std::string& ToStr(const BasicType& type)
{
  static const std::array<std::string, TypesCount> kNames = {
      "bool", "char",
      "int8", "uint8",
      "int16", "uint16",
      "int32", "uint32",
      "int64", "uint64",
      "float32", "float64",
      "other"
  };
  return kNames[static_cast<size_t>(type)];
}

inline bool GetBit(BufferSpan mask, size_t index)
{
  const uint8_t& byte = mask.data[index >> 3];
  return 0 != (byte & uint8_t(1 << (index % 8)));
}

inline void AddFieldToHash(const Schema::Field &field, uint64_t &hash)
{
  const std::hash<std::string> str_hasher;
  const std::hash<BasicType> type_hasher;
  const std::hash<bool> bool_hasher;
  const std::hash<uint16_t> uint_hasher;

  hash ^= str_hasher(field.name) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  hash ^= type_hasher(field.type) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  hash ^= bool_hasher(field.is_vector) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
  hash ^= uint_hasher(field.array_size) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
}

bool Schema::Field::operator==(const Field &other) const
{
  return is_vector == other.is_vector &&
         type == other.type &&
         array_size == other.array_size &&
         name == other.name;
}

inline std::optional<Schema> BuilSchemaFromText(const std::string& txt)
{
  auto startWith = [](const std::string& str, const std::string& match) -> bool
  {
    auto pos = str.find(match);
    return pos == 0;
  };
  auto trimString = [](std::string& str)
  {
    while(str.back() == ' ' || str.back() == '\r') {
      str.pop_back();
    }
    while(str.front() == ' ' || str.front() == '\r') {
      str.erase(0, 1);
    }
  };

  std::istringstream ss(txt);
  std::string line;
  Schema schema;
  while (std::getline(ss, line))
  {
    trimString(line);
    if(line.empty())
    {
      continue;
    }
    Schema::Field field;

    for(size_t i=0; i<TypesCount; i++)
    {
      auto type = static_cast<BasicType>(i);
      const auto& type_name = ToStr(type);
      if(startWith(line, type_name))
      {
        field.type = type;
        break;
      }
    }
    if(field.type == BasicType::OTHER)
    {
      continue;
    }

    auto offset = ToStr(field.type).size();
    if(line[offset]=='[')
    {
      field.is_vector = true;
      auto pos = line.find(']', offset);
      if(pos != offset+1)
      {
        // get number
        std::string sub_string = line.substr(offset+1, pos - offset);
        try {
          field.array_size = std::stoi(sub_string);
        }
        catch(...)
        {
          return std::nullopt;
        }
      }
    }
    offset = line.find(' ', offset);
    field.name = line.substr(offset + 1, line.size() - offset -1);
    trimString(field.name);

    // update the hash
    AddFieldToHash(field, schema.hash);

    schema.fields.push_back(field);
  }
  return schema;
}

template <typename Callback> inline
bool ParseSnapshot(const Schema& schema,
                   SnapshotView snapshot,
                   Callback& callback)
{
  if(schema.hash != snapshot.schema_hash)
  {
    return false;
  }
  BufferSpan buffer = snapshot.payload;

  for(size_t i=0; i<schema.fields.size(); i++)
  {
    const auto& field = schema.fields[i];
    if(GetBit(snapshot.active_mask, i))
    {
      if(!field.is_vector)
      {
        // regular field, not vector/array
        const auto var = DeserializeToVarNumber(field.type, buffer);
        callback(field.name, var);
      }
      else
      {
        uint32_t vect_size = field.array_size;
        if(field.array_size == 0) // dynamic vector
        {
          vect_size = Deserialize<uint32_t>(buffer);
        }
        for(size_t a=0; a<vect_size; a++)
        {
          thread_local std::string tmp_name;
          tmp_name = field.name + "[" + std::to_string(a) + "]";

          const auto var = DeserializeToVarNumber(field.type, buffer);
          callback(tmp_name, var);
        }
      }
    }
  }
  return true;
}

}  // namespace DataTamer
