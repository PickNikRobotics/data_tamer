#include "data_tamer/custom_types.hpp"

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

// Sometimes a field of a type is hidden as a private member and can be
// accesses only by const reference.
// This case is also supported but must be registered differently (see below).
// Classese of this type are commonly found in libraries like Eigen
class Vector2d
{
  double _x = 0;
  double _y = 0;

public:
  Vector2d() = default;
  Vector2d(double x, double y) : _x(x), _y(y) {}

  const double& x() const { return _x; }
  const double& y() const { return _y; }

  double& x() { return _x; }
  double& y() { return _y; }
};

namespace DataTamer
{

template <>
struct TypeDefinition<Point3D>
{
  std::string typeName() const { return "Point3D"; }

  template <class Function>
  void typeDef(Function& addField)
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

  template <class Function>
  void typeDef(Function& addField)
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

  template <class Function>
  void typeDef(Function& addField)
  {
    addField("position", &Pose::pos);
    addField("rotation", &Pose::rot);
  }
};

template <>
struct TypeDefinition<Vector2d>
{
  std::string typeName() const { return "Vector2d"; }

  // typeDef must use a different overload and x() and y() must return const reference to
  // a class attribute
  template <class Function>
  void typeDef(const Vector2d& vect, Function& addField)
  {
    addField("x", &vect.x());
    addField("y", &vect.y());
  }
};

}   // namespace DataTamer
