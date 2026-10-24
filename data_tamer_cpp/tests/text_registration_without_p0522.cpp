// Compiled with GCC's -fno-new-ttp-matching, which matches template template parameters
// as Clang 18 and older do (without P0522): std::basic_string, with three template
// parameters, then does not match `template <class, class> class Container`. Every way
// to register text must compile all the same (tests/CMakeLists.txt).
#include "data_tamer_v2/channel.hpp"

#include <array>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct Labeled
{
  std::string label;
  std::vector<std::string> tags;
};
template <class AddField>
std::string_view TypeDefinition(Labeled& value, AddField& add)
{
  add("label", &value.label);
  add("tags", &value.tags);
  return "Labeled";
}
}  // namespace

void registerText(DataTamer2::LogChannel& channel)
{
  static std::string text;
  static std::string_view view;
  static std::pmr::string pmr;
  static std::vector<std::string> list;
  static std::array<std::string, 2> pair;
  static Labeled labeled;
  channel.registerValue("text", &text);
  channel.registerValue("view", &view);
  channel.registerValue("pmr", &pmr);
  channel.registerValue("list", &list);
  channel.registerValue("pair", &pair);
  channel.registerValue("labeled", &labeled);
  auto logged = channel.createLoggedValue<std::string>("logged");
}
