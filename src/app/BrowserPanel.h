#pragma once

#include <core/Sketch.h>

#include <QWidget>

class Document;
class QTreeWidget;
class QTreeWidgetItem;
class SketchView;

// A Fusion-style browser: the drawing's layer, and under it its sublayers. Each sublayer row has an
// eye (show/hide) and a marker for the active one (click it, or the name, to edit that sublayer:
// the others show faded, as references). Double-click a name (or F2) to rename; the + button and
// the right-click menu add, delete, reorder, and move the selection into a sublayer.
class BrowserPanel : public QWidget
{
    Q_OBJECT

public:
    BrowserPanel(Document& doc, SketchView& view, QWidget* parent = nullptr);

    // The row for a sublayer (for tests).
    QTreeWidgetItem* rowFor(core::Id sublayer) const;

    // Actions, as the buttons and menu do them.
    void addSublayer();
    void deleteSublayer(core::Id sublayer, bool ask = true);
    void moveSelectionTo(core::Id sublayer);

private:
    void rebuild();
    void showMenu(const QPoint& at);

    Document& m_doc;
    SketchView& m_view;
    QTreeWidget* m_tree;
    QTreeWidgetItem* m_layer = nullptr;
    bool m_rebuilding = false;
    std::vector<core::Sublayer> m_shown;  // what the tree shows (to skip needless rebuilds)
    core::Id m_shownActive = core::kNoId;
};
