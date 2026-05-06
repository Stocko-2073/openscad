#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

class Value;

class SolutionType
{
public:
  using Point2d = std::array<double, 2>;

  struct Data {
    bool solved = false;
    int result_code = -1;
    double residual = 0.0;
    int dof = 0;
    std::map<std::string, Point2d> points;
    std::vector<std::string> ordered_names;
    std::vector<std::string> failed_constraints;
  };

  explicit SolutionType(std::shared_ptr<Data> data) : data_(std::move(data)) {}

  Value operator==(const SolutionType& other) const;
  Value operator!=(const SolutionType& other) const;
  Value operator<(const SolutionType& other) const;
  Value operator>(const SolutionType& other) const;
  Value operator<=(const SolutionType& other) const;
  Value operator>=(const SolutionType& other) const;

  [[nodiscard]] bool solved() const { return data_->solved; }
  [[nodiscard]] int result_code() const { return data_->result_code; }
  [[nodiscard]] double residual() const { return data_->residual; }
  [[nodiscard]] int dof() const { return data_->dof; }
  [[nodiscard]] std::optional<Point2d> point(const std::string& name) const;
  [[nodiscard]] const std::vector<std::string>& failed_constraints() const
  {
    return data_->failed_constraints;
  }
  [[nodiscard]] const std::map<std::string, Point2d>& points() const { return data_->points; }
  [[nodiscard]] const std::vector<std::string>& ordered_names() const { return data_->ordered_names; }

private:
  std::shared_ptr<Data> data_;
};

std::ostream& operator<<(std::ostream& stream, const SolutionType& s);
