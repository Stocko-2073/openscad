#pragma once

#include "UXTest.h"

// Incremental evaluation (core/EvalMemo.h) as the window uses it from render to render.
class TestEvalMemo : public UXTest
{
  Q_OBJECT;

private slots:
  void initTestCase();
  void reusesAcrossRenders();
  void flushAndPreferenceDropTheTable();
  void animationTimeIsADependency();
  void framesStartFromTheDocumentsTable();
  void keepsTheRendererOfAnUnchangedResult();
  void benchmarkEditSequence();
  void benchmarkAnimationFirstPass();
};
