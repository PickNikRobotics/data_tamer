#pragma once

#include <cstdint>
#include <string>

namespace DataTamer {

enum class BasicType {
  BOOL,
  BYTE,
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

constexpr size_t TypesCount = 14;

/// Return the number of bytes needed to serialize the type
size_t SizeOf(const BasicType& type);

/// Return the name of the type
char const* ToStr(const BasicType& type);

/// Convert string to its type
BasicType FromStr(const std::string &str);

template <typename T>
inline constexpr BasicType GetBasicType() {
  if constexpr (std::is_same_v<T, char>) return BasicType::BOOL;
  if constexpr (std::is_same_v<T, std::byte>) return BasicType::BYTE;
  if constexpr (std::is_same_v<T, char>) return BasicType::CHAR;
  if constexpr (std::is_same_v<T, std::int8_t>) return BasicType::INT8;
  if constexpr (std::is_same_v<T, std::uint8_t>) return BasicType::UINT8;

  if constexpr (std::is_same_v<T, std::int16_t>) return BasicType::INT16;
  if constexpr (std::is_same_v<T, std::uint16_t>) return BasicType::UINT16;

  if constexpr (std::is_same_v<T, std::int32_t>) return BasicType::INT32;
  if constexpr (std::is_same_v<T, std::uint32_t>) return BasicType::UINT32;

  if constexpr (std::is_same_v<T, std::uint64_t>) return BasicType::UINT64;
  if constexpr (std::is_same_v<T, std::int64_t>) return BasicType::INT64;

  if constexpr (std::is_same_v<T, float>) return BasicType::FLOAT32;
  if constexpr (std::is_same_v<T, double>) return BasicType::FLOAT64;

  return BasicType::OTHER;
}


struct RegistrationID
{
  size_t first_index = 0;
  size_t fields_count = 0;

  void operator+=(const RegistrationID& other) {
    fields_count += other.fields_count;
  }
};

}  // namespace DataTamer
