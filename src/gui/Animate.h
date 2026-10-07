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
  // Frees the renderers kept for frames already shown, as when their colors go out of date.
  void dropFrameRenderers();
  // Drops the memo tables the frames are evaluated with, as MainWindow::dropMemoTables() drops
  // the documents'.
  void dropMemoTables();
#ifdef ENABLE_GUI_TESTS
  OpenScad::Animate::FrameCache *frameCache() { return frameCache_.get(); }
#endif

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

  // Seeds the cache with the current source and step count, unless its frames are of both already:
  // they stay valid across a pause or a change of speed.
  void seedFrameCache();
  void rebuildFrameCacheSource();
  bool tryShowCachedFrame(int step);
  // Shows a ready frame, with the renderer made when it was last shown if there is one.
  void showFrame(const OpenScad::Animate::CachedFrame& frame);
  // Button-driven frame navigation (step/jump): try the prefetch cache first and
  // only sync-render on a miss, then warm the neighbourhood around the new step.
  void showCurrentStepFromCache();

  double animTVal;
  bool animDumping;
  int animDumpStartStep;
  int animStep;
  int animNumSteps;
  // True while incrementTVal is updating e_tval — suppresses the synchronous
  // actionRender path so the prefetch cache can handle the frame.
  bool inTimerTick_ = false;
  // True while a navigation BUTTON is updating e_tval — same idea as
  // inTimerTick_, so the button path can consult the cache before recompiling.
  bool inButtonStep_ = false;

  // Highest step we have actually painted in the current cycle. Out-of-order
  // completions from the parallel worker pool can deliver an older frame after
  // a newer one has already been drawn; we drop those rather than flick back.
  // Reset on rebuildFrameCacheSource and when animStep wraps backwards.
  int lastShownStep_ = -1;

  std::unique_ptr<OpenScad::Animate::FrameCache> frameCache_;
  // Last SourceFile we handed to the cache. We watch this to notice when the
  // script gets re-parsed (auto-reload, post-edit recompile) so the cache can
  // be re-seeded with the new AST.
  std::weak_ptr<class SourceFile> cachedSource_;
  // The step count the cache was seeded with; 0 when it holds no frames.
  int cachedSteps_ = 0;

  // The renderers made for frames already shown, by step, so that showing a frame again draws the
  // buffers it built then: building them takes ~0.1 s for a large design, longer than a frame lasts
  // at most speeds. Only the GUI thread touches them, as GL resources must be freed with the view's
  // context.
  struct FrameRenderer {
    std::weak_ptr<const OpenScad::Animate::FrameResult> frame;  // what it was made from
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
