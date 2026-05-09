#include "gui/Animate.h"

#include <QAction>
#include <QBoxLayout>
#include <QFormLayout>
#include <QIcon>
#include <QList>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QWidget>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "gui/AnimateFrameCache.h"
#include "gui/MainWindow.h"
#include "gui/UIUtils.h"
#include "openscad_gui.h"
#include "utils/printutils.h"

Animate::Animate(QWidget *parent) : QWidget(parent)
{
  setupUi(this);
  initGUI();

  const auto width = animateParameters->minimumSizeHint().width();
  const auto margins = layout()->contentsMargins();
  const auto scrollMargins = scrollAreaWidgetContents->layout()->contentsMargins();
  const auto parameterMargins = animateParameters->layout()->contentsMargins();
  initMinWidth = width + margins.left() + margins.right() + scrollMargins.left() +
                 scrollMargins.right() + parameterMargins.left() + parameterMargins.right();
}

void Animate::initGUI()
{
  this->animStep = 0;
  this->animNumSteps = 0;
  this->animTVal = 0.0;
  this->animDumping = false;
  this->animDumpStartStep = 0;

  this->iconRun = QIcon::fromTheme("chokusen-animate-play");
  this->iconPause = QIcon::fromTheme("chokusen-animate-pause");
  this->iconDisabled = QIcon::fromTheme("chokusen-animate-disabled");

  animateTimer = new QTimer(this);
  connect(animateTimer, &QTimer::timeout, this, &Animate::incrementTVal);

  frameCache_ = std::make_unique<OpenScad::Animate::FrameCache>(this);
  connect(frameCache_.get(), &OpenScad::Animate::FrameCache::frameReady,
          this, &Animate::onFrameReady);
}

void Animate::setMainWindow(MainWindow *mainWindow)
{
  this->mainWindow = mainWindow;

  connectAction(this->actionAnimationPauseUnpause, pauseButton);
  connectAction(this->actionAnimationStart, pushButton_MoveToBeginning);
  connectAction(this->actionAnimationStepBack, pushButton_StepBack);
  connectAction(this->actionAnimationStepForward, pushButton_StepForward);
  connectAction(this->actionAnimationEnd, pushButton_MoveToEnd);
  updatePauseButtonIcon();
}

void Animate::connectAction(QAction *action, QPushButton *button)
{
  connect(action, &QAction::triggered, button, &QPushButton::click);
  this->actionList.append(action);
}

void Animate::on_e_tval_textChanged(const QString&)
{
  double t = this->e_tval->text().toDouble(&this->tOK);
  // Clamp t to 0-1
  if (this->tOK) {
    t = t < 0 ? 0.0 : ((t > 1.0) ? 1.0 : t);
  } else {
    t = 0.0;
  }

  this->animTVal = t;
  // During timer-driven playback, the prefetch cache delivers the frame via
  // onFrameReady. Skip the synchronous render to avoid double work and the
  // GuiLocker contention that drops frames. The text-change still fires from
  // user scrubbing — fall through to the sync path then.
  if (!this->inTimerTick_) {
    emit mainWindow->actionRenderPreview();
  }

  updatePauseButtonIcon();
}

void Animate::on_e_fps_textChanged(const QString&)
{
  updatedAnimFpsAndAnimSteps();
}

void Animate::on_e_fsteps_textChanged(const QString&)
{
  updatedAnimFpsAndAnimSteps();
}

void Animate::updatedAnimFpsAndAnimSteps()
{
  animateTimer->stop();

  int numsteps = this->e_fsteps->text().toInt(&this->steps_ok);
  if (this->steps_ok) {
    this->animNumSteps = numsteps;
  } else {
    this->animNumSteps = 0;
  }
  this->animDumping = false;

  double fps = this->e_fps->text().toDouble(&this->fpsOK);
  animateTimer->stop();
  if (this->fpsOK && fps > 0 && this->animNumSteps > 0) {
    this->animStep = int(this->animTVal * this->animNumSteps) % this->animNumSteps;
    animateTimer->setSingleShot(false);
    animateTimer->setInterval(int(1000 / fps));
    animateTimer->start();
    rebuildFrameCacheSource();
  } else if (frameCache_) {
    frameCache_->invalidateAll();
  }

  QPalette defaultPalette;
  const auto bgColor = defaultPalette.base().color().toRgb();
  QString redStyleSheet = UIUtils::blendForBackgroundColorStyleSheet(bgColor, errorBlendColor);

  if (this->steps_ok || this->e_fsteps->text() == "") {
    this->e_fsteps->setStyleSheet("");
  } else {
    this->e_fsteps->setStyleSheet(redStyleSheet);
  }

  if (this->fpsOK || this->e_fps->text() == "") {
    this->e_fps->setStyleSheet("");
  } else {
    this->e_fps->setStyleSheet(redStyleSheet);
  }

  updatePauseButtonIcon();
}

void Animate::on_e_dump_toggled(bool checked)
{
  if (!checked) this->animDumping = false;

  updatePauseButtonIcon();
}

// Only called from animate_timer
void Animate::incrementTVal()
{
  if (this->animNumSteps == 0) return;

  if (mainWindow->parameterDock->isVisible()) {
    if (mainWindow->activeEditor->parameterWidget->childHasFocus()) return;
  }

  const int prevStep = this->animStep;
  if (this->animNumSteps > 1) {
    this->animStep = (this->animStep + 1) % this->animNumSteps;
    this->animTVal = 1.0 * this->animStep / this->animNumSteps;
  } else if (this->animNumSteps > 0) {
    this->animStep = 0;
    this->animTVal = 0.0;
  }
  // Cycle wrap (or any backwards jump): old "highest shown" no longer applies.
  if (this->animStep < prevStep) lastShownStep_ = -1;

  // Update the time field, but suppress the synchronous render hop — we want
  // the prefetch cache to drive the draw when possible.
  this->inTimerTick_ = true;
  const QString txt = QString::number(this->animTVal, 'f', 5);
  this->e_tval->setText(txt);
  this->inTimerTick_ = false;

  // Dump-pictures mode runs single-threaded for deterministic frame output.
  if (this->dumpPictures()) {
    emit mainWindow->actionRenderPreview();
    updatePauseButtonIcon();
    return;
  }

  if (frameCache_) {
    // If the source has been re-parsed (auto-reload, post-edit recompile),
    // seed the cache with the new AST before trying to draw.
    auto cached = cachedSource_.lock();
    if (mainWindow->rootFile && cached != mainWindow->rootFile) {
      rebuildFrameCacheSource();
    }
    // Try the exact requested step first. If it's not ready, fall back to the
    // freshest Ready frame in the cache — for scenes whose compute time
    // exceeds 1/fps this keeps something visible (slowed playback) instead of
    // a blank GLView until workers catch up. We can't sync-fallback to
    // actionRenderPreview here because that takes GuiLocker, calls
    // instantiateRoot which sets the renderer to nullptr, and stomps on the
    // in-flight prefetch chain.
    if (!tryShowCachedFrame(this->animStep)) {
      auto fallback = frameCache_->latestReady();
      if (fallback && fallback->step != lastShownStep_) {
        mainWindow->showAnimationFrame(fallback->result);
        lastShownStep_ = fallback->step;
      }
    } else {
      lastShownStep_ = this->animStep;
    }
    // Always refill the prefetch window starting one ahead of the current step.
    const int lookahead = frameCache_->workerCount();
    frameCache_->prefetchWindow(this->animStep + 1, lookahead);
  } else {
    emit mainWindow->actionRenderPreview();
  }

  updatePauseButtonIcon();
}

bool Animate::tryShowCachedFrame(int step)
{
  if (!frameCache_ || !mainWindow) return false;
  auto frame = frameCache_->tryGet(step);
  if (!frame) return false;
  const auto state = frame->state.load(std::memory_order_acquire);
  if (state != OpenScad::Animate::FrameState::Ready) return false;
  if (!frame->result) return false;
  mainWindow->showAnimationFrame(frame->result);
  return true;
}

void Animate::onFrameReady(int step)
{
  if (!animateTimer->isActive()) return;
  // Don't backtrack: if a slower worker finishes a step we already rendered
  // past in this cycle, skip it. Otherwise show the just-finished frame —
  // for scenes where compute > 1/fps this is the only path that ever paints,
  // because by the time a worker finishes step N animStep has already moved
  // past it. (Previously we required step == animStep here, which guaranteed
  // a blank GLView for any non-trivial scene.)
  if (lastShownStep_ != -1 && step <= lastShownStep_) return;
  if (tryShowCachedFrame(step)) {
    lastShownStep_ = step;
  }
}

void Animate::rebuildFrameCacheSource()
{
  if (!frameCache_ || !mainWindow) return;
  lastShownStep_ = -1;
  if (this->animNumSteps <= 0) {
    frameCache_->invalidateAll();
    cachedSource_.reset();
    return;
  }
  if (!mainWindow->rootFile) {
    frameCache_->invalidateAll();
    cachedSource_.reset();
    return;
  }
  const std::string docPath = std::filesystem::path(
                                mainWindow->activeEditor->filepath.toStdString())
                                .parent_path()
                                .string();
  frameCache_->setSource(mainWindow->rootFile, docPath, this->animNumSteps,
                         mainWindow->qglview->cam, mainWindow->isPreview);
  cachedSource_ = mainWindow->rootFile;
  frameCache_->prefetchWindow(this->animStep, frameCache_->workerCount());
}

void Animate::updateTVal()
{
  if (this->animNumSteps == 0) return;

  if (this->animStep < 0) {
    this->animStep = this->animNumSteps - this->animStep - 2;
  }

  if (this->animNumSteps > 1) {
    this->animStep = (this->animStep) % this->animNumSteps;
    this->animTVal = 1.0 * this->animStep / this->animNumSteps;
  } else if (this->animNumSteps > 0) {
    this->animStep = 0;
    this->animTVal = 0.0;
  }

  const QString txt = QString::number(this->animTVal, 'f', 5);
  this->e_tval->setText(txt);

  updatePauseButtonIcon();
}

void Animate::pauseAnimation()
{
  animateTimer->stop();
  updatePauseButtonIcon();
}

void Animate::on_pauseButton_pressed()
{
  if (animateTimer->isActive()) {
    animateTimer->stop();
    updatePauseButtonIcon();
  } else {
    this->updatedAnimFpsAndAnimSteps();
  }
}

void Animate::updatePauseButtonIcon()
{
  if (animateTimer->isActive()) {
    pauseButton->setIcon(this->iconPause);
    pauseButton->setToolTip(_("press to pause animation"));
  } else {
    if (this->fpsOK && this->steps_ok) {
      pauseButton->setIcon(this->iconRun);
      pauseButton->setToolTip(_("press to start animation"));
    } else {
      pauseButton->setIcon(this->iconDisabled);
      pauseButton->setToolTip(_("incorrect values"));
    }
  }
}

void Animate::cameraChanged()
{
  this->animateUpdate();  // for now so that we do not change the behavior
}

void Animate::editorContentChanged()
{
  if (frameCache_) frameCache_->invalidateAll();
  if (animateTimer && animateTimer->isActive()) rebuildFrameCacheSource();
  this->animateUpdate();  // for now so that we do not change the behavior
}

void Animate::animateUpdate()
{
  if (mainWindow->animateDockContents->isVisible()) {
    double fps = this->e_fps->text().toDouble(&this->fpsOK);
    if (this->fpsOK && fps <= 0 && !animateTimer->isActive()) {
      animateTimer->stop();
      animateTimer->setSingleShot(true);
      animateTimer->setInterval(50);
      animateTimer->start();
    }
  }
}

bool Animate::dumpPictures()
{
  return this->e_dump->isChecked() && this->animateTimer->isActive();
}

int Animate::nextFrame()
{
  if (animDumping && animDumpStartStep == animStep) {
    animDumping = false;
    e_dump->setChecked(false);
  } else {
    if (!animDumping) {
      animDumping = true;
      animDumpStartStep = animStep;
    }
  }
  return animStep;
}

void Animate::resizeEvent(QResizeEvent *event)
{
  auto layoutParameters = dynamic_cast<QBoxLayout *>(animateParameters->layout());
  auto layoutButtons = dynamic_cast<QBoxLayout *>(animateButtons->layout());

  if (layoutParameters && layoutButtons) {
    if (layoutParameters->direction() == QBoxLayout::LeftToRight) {
      if (event->size().width() < initMinWidth) {
        layoutParameters->setDirection(QBoxLayout::TopToBottom);
        layoutButtons->setDirection(QBoxLayout::TopToBottom);
        scrollAreaWidgetContents->layout()->invalidate();
      }
    } else {
      if (event->size().width() > initMinWidth) {
        layoutParameters->setDirection(QBoxLayout::LeftToRight);
        layoutButtons->setDirection(QBoxLayout::LeftToRight);
        scrollAreaWidgetContents->layout()->invalidate();
      }
    }
  }

  QWidget::resizeEvent(event);
}

const QList<QAction *>& Animate::actions()
{
  return actionList;
}

void Animate::onActionEvent(InputEventAction *event)
{
  const std::string actionString = event->action;
  const std::string actionName = actionString.substr(actionString.find("::") + 2, std::string::npos);
  for (auto action : actionList) {
    if (actionName == action->objectName().toStdString()) {
      action->trigger();
    }
  }
}

double Animate::getAnimTval()
{
  return animTVal;
}

void Animate::on_pushButton_MoveToBeginning_clicked()
{
  pauseAnimation();
  this->animStep = 0;
  this->updateTVal();
}

void Animate::on_pushButton_StepBack_clicked()
{
  pauseAnimation();
  this->animStep -= 1;
  this->updateTVal();
}

void Animate::on_pushButton_StepForward_clicked()
{
  pauseAnimation();
  this->animStep += 1;
  this->updateTVal();
}

void Animate::on_pushButton_MoveToEnd_clicked()
{
  pauseAnimation();
  this->animStep = this->animNumSteps - 1;
  this->updateTVal();
}
