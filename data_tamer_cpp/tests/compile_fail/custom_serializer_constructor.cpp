// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// A type name is not a serializer: CustomSerializerT must be built from it explicitly.
// As written the serializer is built with the name directly; DT_IMPLICIT_NAME converts
// a std::string into one.
#include "data_tamer_v2/custom_types.hpp"
#include "described_type.hpp"

#include <memory>
#include <string>

namespace probe
{
std::shared_ptr<DataTamer2::CustomSerializer> make()
{
  DataTamer2::CustomSerializerT<Described> direct(std::string("Described"));
#ifdef DT_IMPLICIT_NAME
  DataTamer2::CustomSerializerT<Described> implicit = std::string("Described");
  (void)implicit;
#endif
  return std::make_shared<DataTamer2::CustomSerializerT<Described>>(std::string("Named"));
}
}  // namespace probe
