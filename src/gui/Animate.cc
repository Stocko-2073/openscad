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

#include "core/EvalMemo.h"
#include "geometry/Geometry.h"
#include "geometry/PolySet.h"
#include "glview/Renderer.h"
#include "gui/AnimateFrameCache.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/UIUtils.h"
#include "openscad_gui.h"
#include "utils/printutils.h"

namespace {

constexpr size_t kMaxFrameRendererBytes = size_t{2} << 30;

// Buffers of 48 bytes for each corner of each triangle, and the mesh kept for measuring.
size_t estimatedRendererBytes(const OpenScad::Animate::FrameResult& frame)
{
  size_t facets = frame.geometry ? frame.geometry->numFacets() : 0;
  for (const auto& mesh : frame.overlays) {
    if (mesh.polyset) facets += mesh.polyset->numFacets();
  }
  return facets * (3 * 48 + 28);
}

}  // namespace

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
  // The tick and the step buttons show the frame themselves.
  if (!this->inTimerTick_ && !this->inButtonStep_) {
    mainWindow->actionRender();
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
    seedFrameCache();
  } else if (this->animNumSteps <= 0) {
    // No steps, no frames. A pause or a speed being typed keeps them.
    rebuildFrameCacheSource();
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
  // Dumping renders every step in turn: wait until the last one is rendered and saved.
  if (this->dumpPictures() && GuiLocker::isLocked()) return;

  const int prevStep = this->animStep;
  if (this->animNumSteps > 1) {
    this->animStep = (this->animStep + 1) % this->animNumSteps;
    this->animTVal = 1.0 * this->animStep / this->animNumSteps;
  } else if (this->animNumSteps > 0) {
    this->animStep = 0;
    this->animTVal = 0.0;
  }
  if (this->animStep < prevStep) lastShownStep_ = -1;

  this->inTimerTick_ = true;
  const QString txt = QString::number(this->animTVal, 'f', 5);
  this->e_tval->setText(txt);
  this->inTimerTick_ = false;

  if (this->dumpPictures()) {
    mainWindow->actionRender();
    updatePauseButtonIcon();
    return;
  }

  if (frameCache_) {
    auto cached = cachedSource_.lock();
    if (mainWindow->rootFile && cached != mainWindow->rootFile) {
      rebuildFrameCacheSource();
    }
    // A miss shows the latest ready frame, not a render: a render parses the source again, and a
    // new source empties the cache.
    if (!tryShowCachedFrame(this->animStep)) {
      auto fallback = frameCache_->latestReady();
      if (fallback && fallback->step != lastShownStep_) {
        showFrame(*fallback);
        lastShownStep_ = fallback->step;
      }
    } else {
      lastShownStep_ = this->animStep;
    }
    const int lookahead = frameCache_->workerCount();
    frameCache_->prefetchWindow(this->animStep + 1, lookahead);
  } else {
    mainWindow->actionRender();
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
  showFrame(*frame);
  return true;
}

void Animate::showFrame(const OpenScad::Animate::CachedFrame& frame)
{
  const auto it = frameRenderers_.find(frame.step);
  if (it != frameRenderers_.end()) {
    if (it->second.frame.lock() == frame.result) {
      mainWindow->showAnimationFrame(it->second.renderer);
      return;
    }
    frameRendererBytes_ -= it->second.bytes;
    frameRenderers_.erase(it);
  }
  auto renderer = mainWindow->createFrameRenderer(*frame.result);
  const size_t bytes = estimatedRendererBytes(*frame.result);
  if (renderer && frameRendererBytes_ + bytes <= kMaxFrameRendererBytes) {
    frameRenderers_.emplace(frame.step, FrameRenderer{frame.result, renderer, bytes});
    frameRendererBytes_ += bytes;
  }
  mainWindow->showAnimationFrame(renderer);
}

void Animate::dropFrameRenderers()
{
  if (frameRenderers_.empty()) return;
  // Their buffers are freed with the view's GL context current, as outside a paint it may not be.
  QOpenGLContext *oldContext = getGLContext();
  mainWindow->qglview->makeCurrent();
  frameRenderers_.clear();
  mainWindow->qglview->doneCurrent();
  setGLContext(oldContext);
  frameRendererBytes_ = 0;
}

void Animate::dropMemoTables()
{
  if (frameCache_) frameCache_->dropMemoTables();
}

void Animate::showCurrentStepFromCache()
{
  if (this->animNumSteps == 0) return;

  this->inButtonStep_ = true;
  this->updateTVal();
  this->inButtonStep_ = false;

  if (!frameCache_) {
    mainWindow->actionRender();
    return;
  }

  auto cached = cachedSource_.lock();
  if (!cached || cached != mainWindow->rootFile) {
    rebuildFrameCacheSource();
  }

  if (tryShowCachedFrame(this->animStep)) {
    mainWindow->resetMeasurementsState(true, _("Click to start measuring"));
  } else {
    mainWindow->actionRender();
  }

  // Centred on the step: the buttons step back as well as forward.
  const int lookahead = frameCache_->workerCount();
  frameCache_->prefetchWindow(this->animStep - lookahead / 2, lookahead);

  updatePauseButtonIcon();
}

void Animate::onFrameReady(int step)
{
  // Paused, the current step is already drawn.
  if (!animateTimer->isActive()) return;
  // Not only animStep's frame: one slower than a tick is ready only once animStep has moved past it.
  if (lastShownStep_ != -1 && step <= lastShownStep_) return;
  if (tryShowCachedFrame(step)) {
    lastShownStep_ = step;
  }
}

void Animate::seedFrameCache()
{
  if (!frameCache_ || !mainWindow) return;
  if (this->animNumSteps > 0 && mainWindow->rootFile && cachedSource_.lock() == mainWindow->rootFile &&
      cachedSteps_ == this->animNumSteps) {
    lastShownStep_ = -1;
    frameCache_->prefetchWindow(this->animStep, frameCache_->workerCount());
    return;
  }
  rebuildFrameCacheSource();
}

void Animate::rebuildFrameCacheSource()
{
  if (!frameCache_ || !mainWindow) return;
  lastShownStep_ = -1;
  dropFrameRenderers();
  cachedSteps_ = 0;
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
  // The frames' memo tables start as copies of the document's, unless a render is using it now.
  const memo::MemoTable *seed =
    GuiLocker::isLocked() ? nullptr : mainWindow->activeEditor->memoTable.get();
  frameCache_->setSource(mainWindow->rootFile, docPath, this->animNumSteps,
                         mainWindow->qglview->cam, seed);
  cachedSource_ = mainWindow->rootFile;
  cachedSteps_ = this->animNumSteps;
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
  // The frames are of the source last parsed, as the view is, and stay until a render parses the edit.
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
  this->showCurrentStepFromCache();
}

void Animate::on_pushButton_StepBack_clicked()
{
  pauseAnimation();
  this->animStep -= 1;
  this->showCurrentStepFromCache();
}

void Animate::on_pushButton_StepForward_clicked()
{
  pauseAnimation();
  this->animStep += 1;
  this->showCurrentStepFromCache();
}

void Animate::on_pushButton_MoveToEnd_clicked()
{
  pauseAnimation();
  this->animStep = this->animNumSteps - 1;
  this->showCurrentStepFromCache();
}
