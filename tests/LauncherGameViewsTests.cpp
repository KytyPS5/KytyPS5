#include "gameViews.h"
#include "launcherTheme.h"
#include "ui_configuration_list_widget.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QEvent>
#include <QImage>
#include <QScrollBar>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "LauncherGameViewsTests: failed: %s\n", message);
    std::abort();
  }
}

// The red pic0 dominates a pixel only where nothing opaque is painted over the background.
bool ShowsBackground(const QColor &color) {
  return color.red() > color.green() + 40 && color.red() > color.blue() + 40;
}

// The card panel is the app's (white) Window color over the background.
bool ShowsPanel(const QColor &color) {
  return !ShowsBackground(color) &&
         qAbs(color.lightness() - QApplication::palette().color(QPalette::Window).lightness()) < 32;
}

class PaintCounter final : public QObject {
public:
  int paints = 0;

protected:
  bool eventFilter(QObject * /*watched*/, QEvent *event) override {
    paints += static_cast<int>(event->type() == QEvent::Paint);
    return false;
  }
};

} // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  // Breeze, where installed, paints its own color behind scroll bars; Fusion's are opaque.
  const bool breeze = QApplication::setStyle(QStringLiteral("Breeze")) != nullptr;
  // A fixed light palette, whatever the desktop colors: a black panel then stands out.
  QApplication::setPalette(QPalette(Qt::white));
  LauncherTheme::Initialize(application);

  QTemporaryDir game;
  Check(QDir(game.path()).mkpath(QStringLiteral("sce_sys")), "cannot create sce_sys");
  QImage pic0(64, 36, QImage::Format_RGB32);
  pic0.fill(Qt::red);
  Check(pic0.save(game.filePath(QStringLiteral("sce_sys/pic0.png"))), "cannot write pic0.png");

  QWidget                     host;
  Ui::ConfigurationListWidget ui;
  ui.setupUi(&host);
  auto *views = ui.game_views;
  auto *list  = ui.cfgs_list;
  auto *grid  = ui.cfgs_grid;
  SetupGameGrid(list, grid);
  grid->setIconSize(QSize(64, 64));

  QPixmap cover(64, 64);
  cover.fill(Qt::blue);
  auto *item = new QTreeWidgetItem(list, {QStringLiteral("Game")});
  item->setIcon(GAME_NAME_COLUMN, QIcon(cover));

  host.resize(480, 360);
  views->SetBackgroundImage(game.path());
  host.show();
  QApplication::processEvents();
  const QPoint empty(views->width() - 60, views->height() - 30);

  Check(ShowsBackground(views->grab().toImage().pixelColor(empty)),
        "the game background is hidden behind the list view");

  views->setCurrentWidget(grid);
  QApplication::processEvents();
  const auto image = views->grab().toImage();
  Check(ShowsBackground(image.pixelColor(empty)),
        "the game background is hidden behind the grid view");

  const auto card = grid->visualRect(grid->model()->index(0, GAME_NAME_COLUMN))
                        .translated(grid->viewport()->mapTo(views, QPoint()));
  Check(card.width() == 64 + 16, "the card is not the cover plus its padding wide");
  Check(image.pixelColor(card.topLeft() + QPoint(8 + 32, 8 + 32)) == QColor(Qt::blue),
        "the cover is not drawn inside the card padding");
  Check(ShowsPanel(image.pixelColor(card.topLeft() + QPoint(4, 40))),
        "the card panel does not back its padding");

  PaintCounter counter;
  grid->viewport()->installEventFilter(&counter);
  item->setText(GAME_SIZE_COLUMN, QStringLiteral("1.00 GB"));
  QApplication::processEvents();
  Check(counter.paints > 0, "the card does not repaint when the game size arrives");

  if (breeze) {
    for (QAbstractScrollArea *view : {static_cast<QAbstractScrollArea *>(grid),
                                      static_cast<QAbstractScrollArea *>(list)}) {
      view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
      views->setCurrentWidget(view);
      QApplication::processEvents();
      const auto *bar = view->verticalScrollBar();
      Check(ShowsBackground(views->grab().toImage().pixelColor(
                bar->mapTo(views, QPoint(2, bar->height() - 20)))),
            "Breeze hides the game background behind a scroll bar");
    }
  }

  std::printf("LauncherGameViewsTests: passed%s\n", breeze ? "" : " (Breeze checks skipped)");
  return 0;
}
