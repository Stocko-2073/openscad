#pragma once

#include <QAction>
#include <QColor>
#include <QIcon>
#include <QList>
#include <QPushButton>
#include <QResizeEvent>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

#include "gui/input/InputDriverEvent.h"
#include "gui/qtgettext.h"
#include "ui_Animate.h"

class MainWindow;
class Renderer;
namespace OpenScad::Animate {
class FrameCache;
struct CachedFrame;
struct FrameResult;
}  // namespace OpenScad::Animate

class Animate : public QWidget, public Ui::AnimateWidget
{
  Q_OBJECT

public:
  Animate(QWidget *parent = nullptr);
  Animate(const Animate& source) = delete;
  Animate(Animate&& source) = delete;
  Animate& operator=(const Animate& source) = delete;
  Animate& operator=(Animate&& source) = delete;
  ~Animate() override = default;

  void initGUI();
  bool dumpPictures();
  int nextFrame();

  QTimer *animateTimer;

  void setMainWindow(MainWindow *mainWindow);

  const QList<QAction *>& actions();
  double getAnimTval();
  void dropFrameRenderers();
  void dropMemoTables();

public slots:
  void animateUpdate();
  void cameraChanged();
  void editorContentChanged();
  void onActionEvent(InputEventAction *event);
  void pauseAnimation();

  void on_pushButton_MoveToBeginning_clicked();
  void on_pushButton_StepBack_clicked();
  void on_pushButton_StepForward_clicked();
  void on_pushButton_MoveToEnd_clicked();

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  MainWindow *mainWindow;

  void updatePauseButtonIcon();
  void connectAction(QAction *, QPushButton *);

  // Seeds the cache with the current source and step count, unless its frames are of both already.
  void seedFrameCache();
  void rebuildFrameCacheSource();
  bool tryShowCachedFrame(int step);
  void showFrame(const OpenScad::Animate::CachedFrame& frame);
  void showCurrentStepFromCache();

  double animTVal;
  bool animDumping;
  int animDumpStartStep;
  int animStep;
  int animNumSteps;
  bool inTimerTick_ = false;
  bool inButtonStep_ = false;

  // Highest step shown this cycle. Frames finish out of order; older ones are not shown after it.
  int lastShownStep_ = -1;

  std::unique_ptr<OpenScad::Animate::FrameCache> frameCache_;
  std::weak_ptr<class SourceFile> cachedSource_;
  // The step count the cache was seeded with; 0 when it holds no frames.
  int cachedSteps_ = 0;

  // Renderers of frames already shown, by step: building a frame's buffers can take longer than the
  // frame lasts. GUI thread only, as GL resources must be freed with the view's context.
  struct FrameRenderer {
    std::weak_ptr<const OpenScad::Animate::FrameResult> frame;
    std::shared_ptr<Renderer> renderer;
    size_t bytes;  // estimated
  };
  std::unordered_map<int, FrameRenderer> frameRenderers_;
  size_t frameRendererBytes_ = 0;

  bool fpsOK;
  bool tOK;
  bool steps_ok;

  int initMinWidth;

  QIcon iconRun;
  QIcon iconPause;
  QIcon iconDisabled;
  QList<QAction *> actionList;
  QColor errorBlendColor{"red"};

signals:

private slots:
  void on_e_tval_textChanged(const QString&);
  void on_e_fps_textChanged(const QString&);
  void on_e_fsteps_textChanged(const QString&);
  void on_e_dump_toggled(bool checked);
  void updatedAnimFpsAndAnimSteps();
  void incrementTVal();
  void updateTVal();
  void on_pauseButton_pressed();
  void onFrameReady(int step);
};
