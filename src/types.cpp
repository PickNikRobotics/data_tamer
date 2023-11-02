#include "data_tamer/types.hpp"
#include <array>
#include <unordered_map>

namespace DataTamer {

static constexpr std::array<const char*, TypesCount> kNames = {
    "bool", "byte", "char",
    "int8", "uint8",
    "int16", "uint16",
    "int32", "uint32",
    "int64", "uint64",
    "float32", "float64",
    "other"
};

const char* ToStr(const BasicType& type)
{
  return kNames[static_cast<size_t>(type)];
}

BasicType FromStr(const std::string& str)
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

size_t SizeOf(const BasicType& type)
{
  static constexpr std::array<size_t, TypesCount> kSizes =
      { 1, 1, 1, 1, 1,
       2, 2, 4, 4, 8, 8,
       4, 8, 0 };
  return kSizes[static_cast<size_t>(type)];
}

}  // namespace DataTamer
