#include "core/SolutionType.h"

#include "core/Value.h"

Value SolutionType::operator==(const SolutionType& other) const
{
  return data_.get() == other.data_.get();
}
Value SolutionType::operator!=(const SolutionType& other) const
{
  return data_.get() != other.data_.get();
}
Value SolutionType::operator<(const SolutionType& /*other*/) const
{
  return Value::undef("operation undefined (solution < solution)");
}
Value SolutionType::operator>(const SolutionType& /*other*/) const
{
  return Value::undef("operation undefined (solution > solution)");
}
Value SolutionType::operator<=(const SolutionType& /*other*/) const
{
  return Value::undef("operation undefined (solution <= solution)");
}
Value SolutionType::operator>=(const SolutionType& /*other*/) const
{
  return Value::undef("operation undefined (solution >= solution)");
}

std::optional<SolutionType::Point2d> SolutionType::point(const std::string& name) const
{
  auto it = data_->points.find(name);
  if (it == data_->points.end()) return std::nullopt;
  return it->second;
}

std::ostream& operator<<(std::ostream& stream, const SolutionType& s)
{
  stream << "Solution(solved=" << (s.solved() ? "true" : "false")
         << ", dof=" << s.dof()
         << ", residual=" << s.residual()
         << ", points={";
  bool first = true;
  for (const auto& [name, _] : s.points()) {
    stream << (first ? "" : ",") << name;
    first = false;
  }
  stream << "})";
  return stream;
}
