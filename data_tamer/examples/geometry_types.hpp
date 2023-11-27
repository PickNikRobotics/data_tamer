#include "data_tamer/contrib/SerializeMe.hpp"

struct Point3D
{
  double x;
  double y;
  double z;
};

struct Quaternion
{
  double w;
  double x;
  double y;
  double z;
};

struct Pose
{
  Point3D pos;
  Quaternion rot;
};


namespace SerializeMe
{
template <>
struct TypeDefinition<Point3D>
{
  std::string typeName() const { return "Point3D"; }

  template <class Function> void typeDef(Function& addField)
  {
    addField("x", &Point3D::x);
    addField("y", &Point3D::y);
    addField("z", &Point3D::z);
  }
};

template <>
struct TypeDefinition<Quaternion>
{
  std::string typeName() const { return "Quaternion"; }

  template <class Function> void typeDef(Function& addField)
  {
    addField("w", &Quaternion::w);
    addField("x", &Quaternion::x);
    addField("y", &Quaternion::y);
    addField("z", &Quaternion::z);
  }
};

template <>
struct TypeDefinition<Pose>
{
  std::string typeName() const { return "Pose"; }

  template <class Function> void typeDef(Function& addField)
  {
    addField("position", &Pose::pos);
    addField("rotation", &Pose::rot);
  }
};


} // namespace SerializeMe
