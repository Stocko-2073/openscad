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
#include <memory>
#include <string>

#include "gui/input/InputDriverEvent.h"
#include "gui/qtgettext.h"
#include "ui_Animate.h"

class MainWindow;
namespace OpenScad::Animate { class FrameCache; }

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

  void rebuildFrameCacheSource();
  bool tryShowCachedFrame(int step);
  // Button-driven frame navigation (step/jump): try the prefetch cache first and
  // only sync-render on a miss, then warm the neighbourhood around the new step.
  void showCurrentStepFromCache();

  double animTVal;
  bool animDumping;
  int animDumpStartStep;
  int animStep;
  int animNumSteps;
  // True while incrementTVal is updating e_tval — suppresses the synchronous
  // actionRenderPreview path so the prefetch cache can handle the frame.
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
