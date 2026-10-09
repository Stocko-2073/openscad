#pragma once

#include <vector>

#include <QString>
#include <QWidget>

#include "glview/ColorMap.h"

class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QToolButton;

// Shows a render color scheme, and saves a user scheme as its colors and edge brightness are set.
class ColorSchemeEditor : public QWidget
{
  Q_OBJECT

public:
  explicit ColorSchemeEditor(QWidget *parent = nullptr);
  void setScheme(const QString& name);

signals:
  void schemeEdited(const QString& name);

private:
  struct Row {
    QString title;
    std::vector<RenderColor> colors;  // shows the first, writes them all
    QToolButton *swatch;
    QLabel *hex;
  };

  void addSection(const QString& title);
  void addRow(const QString& title, std::vector<RenderColor> colors, const QString& tooltip = {});
  void pick(const Row& row);
  void setEdgeBrightness(double value);
  bool save(ColorScheme scheme);
  void showColors();

  QGridLayout *grid_;
  int gridRow_ = 0;
  QString section_;
  QLabel *readOnlyNote_;
  std::vector<Row> rows_;
  QDoubleSpinBox *edgeBrightness_;
  QString name_;
  ColorScheme scheme_;
};
