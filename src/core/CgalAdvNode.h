#pragma once

#include <string>

#include "core/ModuleInstantiation.h"
#include "core/node.h"
#include "geometry/linalg.h"

enum class CgalAdvType { MINKOWSKI, HULL, FILL, RESIZE };

class CgalAdvNode : public AbstractNode
{
public:
  VISITABLE();
  CgalAdvNode(const ModuleInstantiation *mi, CgalAdvType type) : AbstractNode(mi), type(type) {}
  std::string toString() const override;
  std::string name() const override;
  std::shared_ptr<AbstractNode> copy() const override { return copyAs(*this); }

  unsigned int convexity{1};
  Vector3d newsize;
  Eigen::Matrix<bool, 3, 1> autosize;
  CgalAdvType type;
};
