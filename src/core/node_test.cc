#include "core/node.h"

#include <catch2/catch_all.hpp>
#include <future>
#include <memory>
#include <thread>

TEST_CASE("A thread's node numbering is its own", "[node]")
{
  // An Animate frame worker builds a tree while the GUI thread starts on one of its own.
  std::promise<void> madeFirst, guiReset;
  int first = 0, second = 0;
  std::thread worker([&] {
    AbstractNode::resetIndexCounter();
    first = std::make_shared<GroupNode>(nullptr)->index();
    madeFirst.set_value();
    guiReset.get_future().wait();
    second = std::make_shared<GroupNode>(nullptr)->index();
  });
  madeFirst.get_future().wait();
  AbstractNode::resetIndexCounter();
  guiReset.set_value();
  worker.join();

  CHECK(first == 1);
  CHECK(second == 2);
  CHECK(std::make_shared<GroupNode>(nullptr)->index() == 1);
}
