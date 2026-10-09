#include "gui/ColorSchemeEditor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <QColor>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFont>
#include <QGridLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include "utils/printutils.h"

namespace {

const QSize SWATCH_SIZE(40, 16);

QColor toQColor(const Color4f& color)
{
  const auto channel = [](float v) {
    return std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255);
  };
  return {channel(color.r()), channel(color.g()), channel(color.b())};
}

// Disabled looks the same, so a read-only scheme still shows its colors.
QIcon swatchIcon(const QColor& color, qreal dpr)
{
  QPixmap pixmap(SWATCH_SIZE * dpr);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(color);
  QPainter painter(&pixmap);
  painter.setPen(QColor(0, 0, 0, 96));
  painter.drawRect(QRectF(0.5, 0.5, SWATCH_SIZE.width() - 1, SWATCH_SIZE.height() - 1));
  painter.end();
  QIcon icon;
  icon.addPixmap(pixmap, QIcon::Normal);
  icon.addPixmap(pixmap, QIcon::Disabled);
  return icon;
}

}  // namespace

ColorSchemeEditor::ColorSchemeEditor(QWidget *parent) : QWidget(parent)
{
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  readOnlyNote_ =
    new QLabel(_("Built-in color schemes are read-only. Duplicate one to edit it."), this);
  readOnlyNote_->setWordWrap(true);
  QSizePolicy notePolicy = readOnlyNote_->sizePolicy();
  notePolicy.setRetainSizeWhenHidden(true);
  readOnlyNote_->setSizePolicy(notePolicy);
  layout->addWidget(readOnlyNote_);

  grid_ = new QGridLayout();
  grid_->setColumnStretch(3, 1);
  layout->addLayout(grid_);
  layout->addStretch();

  addSection(_("View"));
  addRow(_("Background"), {RenderColor::BACKGROUND_COLOR});
  addRow(_("Background gradient"), {RenderColor::BACKGROUND_STOP_COLOR},
         _("The bottom of the background. The same color as Background makes it solid."));
  addRow(_("Axes"), {RenderColor::AXES_COLOR});
  addRow(_("Crosshair"), {RenderColor::CROSSHAIR_COLOR});
  addSection(_("3D"));
  addRow(_("Faces"), {RenderColor::CGAL_FACE_FRONT_COLOR, RenderColor::OPENCSG_FACE_FRONT_COLOR});
  addRow(_("Cut faces"), {RenderColor::CGAL_FACE_BACK_COLOR, RenderColor::OPENCSG_FACE_BACK_COLOR},
         _("The faces left where difference() cuts parts away."));
  const QString edgeTip = _("How Show Edges shades each edge from its face's color: 0 is black, "
                            "0.5 the face's color and 1 white.");
  auto *edgeLabel = new QLabel(_("Edge brightness"), this);
  edgeLabel->setIndent(12);
  edgeLabel->setToolTip(edgeTip);
  edgeBrightness_ = new QDoubleSpinBox(this);
  edgeBrightness_->setRange(0.0, 1.0);
  edgeBrightness_->setSingleStep(0.05);
  edgeBrightness_->setDecimals(2);
  edgeBrightness_->setKeyboardTracking(false);
  edgeBrightness_->setToolTip(edgeTip);
  grid_->addWidget(edgeLabel, gridRow_, 0);
  grid_->addWidget(edgeBrightness_, gridRow_++, 1, 1, 2, Qt::AlignLeft);
  connect(edgeBrightness_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          &ColorSchemeEditor::setEdgeBrightness);
  addSection(_("2D"));
  addRow(_("Faces"), {RenderColor::CGAL_FACE_2D_COLOR});
  addRow(_("Outlines"), {RenderColor::CGAL_EDGE_2D_COLOR});
  addSection(_("CGAL backend only"));
  addRow(_("Edges"), {RenderColor::CGAL_EDGE_FRONT_COLOR});
  addRow(_("Cut edges"), {RenderColor::CGAL_EDGE_BACK_COLOR});

  setScheme({});
}

void ColorSchemeEditor::addSection(const QString& title)
{
  auto *label = new QLabel(title, this);
  QFont font = label->font();
  font.setBold(true);
  label->setFont(font);
  if (gridRow_ > 0) label->setContentsMargins(0, 8, 0, 0);
  grid_->addWidget(label, gridRow_++, 0, 1, 3);
  section_ = title;
}

void ColorSchemeEditor::addRow(const QString& title, std::vector<RenderColor> colors,
                               const QString& tooltip)
{
  auto *label = new QLabel(title, this);
  label->setIndent(12);
  auto *swatch = new QToolButton(this);
  swatch->setIconSize(SWATCH_SIZE);
  auto *hex = new QLabel(this);
  hex->setTextInteractionFlags(Qt::TextSelectableByMouse);
  if (!tooltip.isEmpty()) {
    label->setToolTip(tooltip);
    swatch->setToolTip(tooltip);
  }
  grid_->addWidget(label, gridRow_, 0);
  grid_->addWidget(swatch, gridRow_, 1);
  grid_->addWidget(hex, gridRow_, 2);
  ++gridRow_;

  const size_t index = rows_.size();
  rows_.push_back({QString("%1: %2").arg(section_, title), std::move(colors), swatch, hex});
  connect(swatch, &QToolButton::clicked, this, [this, index]() { pick(rows_[index]); });
}

void ColorSchemeEditor::setScheme(const QString& name)
{
  name_ = name;
  const ColorScheme *scheme = ColorMap::inst()->findColorScheme(name.toStdString());
  scheme_ = scheme ? *scheme : ColorScheme{};
  const bool editable = ColorMap::inst()->isUserColorScheme(name.toStdString());
  readOnlyNote_->setVisible(scheme && !editable);
  for (const auto& row : rows_) row.swatch->setEnabled(editable);
  edgeBrightness_->setReadOnly(!editable);
  const QSignalBlocker blocker(edgeBrightness_);
  edgeBrightness_->setValue(scheme_.edge_brightness);
  showColors();
}

void ColorSchemeEditor::showColors()
{
  for (const auto& row : rows_) {
    const QColor color = toQColor(ColorMap::getColor(scheme_, row.colors.front()));
    row.swatch->setIcon(swatchIcon(color, devicePixelRatioF()));
    row.hex->setText(color.name());
  }
}

void ColorSchemeEditor::pick(const Row& row)
{
  const QColor current = toQColor(ColorMap::getColor(scheme_, row.colors.front()));
  const QColor picked = QColorDialog::getColor(current, this, row.title);
  if (!picked.isValid() || picked == current) return;

  const Color4f color(picked.red(), picked.green(), picked.blue());
  ColorScheme scheme = scheme_;
  // A solid background stays solid.
  if (row.colors.front() == RenderColor::BACKGROUND_COLOR &&
      ColorMap::getColor(scheme_, RenderColor::BACKGROUND_STOP_COLOR) ==
        ColorMap::getColor(scheme_, RenderColor::BACKGROUND_COLOR)) {
    scheme.colors[RenderColor::BACKGROUND_STOP_COLOR] = color;
  }
  for (const auto rc : row.colors) scheme.colors[rc] = color;
  if (save(std::move(scheme))) showColors();
}

void ColorSchemeEditor::setEdgeBrightness(double value)
{
  ColorScheme scheme = scheme_;
  scheme.edge_brightness = value;
  if (!save(std::move(scheme))) {
    const QSignalBlocker blocker(edgeBrightness_);
    edgeBrightness_->setValue(scheme_.edge_brightness);
  }
}

bool ColorSchemeEditor::save(ColorScheme scheme)
{
  const std::string name = name_.toStdString();
  if (const auto error = ColorMap::inst()->saveUserColorScheme(name, name, scheme); !error.empty()) {
    QMessageBox::critical(this, _("Color scheme"), QString::fromStdString(error), QMessageBox::Ok);
    return false;
  }
  scheme_ = std::move(scheme);
  emit schemeEdited(name_);
  return true;
}
