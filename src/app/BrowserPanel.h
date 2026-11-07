#pragma once

#include <core/Sketch.h>

#include <QWidget>

class Document;
class QTreeWidget;
class QTreeWidgetItem;
class SketchView;

// A Fusion-style browser: the drawing's layers (the first on top), and under each its sublayers.
// Each row has an eye (show/hide) and a marker for the active one: click it, or the name, to edit
// that layer / sublayer. Other layers' geometry can't be used except by projecting it (Project);
// other sublayers of the active layer show faded, as references. Double-click a name (or F2) to
// rename; the + button and the right-click menu add, delete, reorder, and move the selection into
// a sublayer. Each layer also lists its reference images (Insert Image): click one to place it,
// the eye to hide it.
class BrowserPanel : public QWidget
{
    Q_OBJECT

public:
    BrowserPanel(Document& doc, SketchView& view, QWidget* parent = nullptr);

    // The rows for a layer / sublayer (for tests).
    QTreeWidgetItem* rowForLayer(core::Id layer) const;
    QTreeWidgetItem* rowFor(core::Id sublayer) const;
    QTreeWidgetItem* rowForImage(core::Id image) const;

    // Actions, as the buttons and menu do them.
    void addLayer();
    void addSublayer();  // in the active layer
    void deleteLayer(core::Id layer, bool ask = true);
    void deleteSublayer(core::Id sublayer, bool ask = true);
    void moveSelectionTo(core::Id sublayer);

private:
    void rebuild();
    void showMenu(const QPoint& at);
    bool isLayerRow(const QTreeWidgetItem* item) const;

    Document& m_doc;
    SketchView& m_view;
    QTreeWidget* m_tree;
    bool m_rebuilding = false;
    QString m_shownKey;  // what the tree shows (to skip needless rebuilds)
};
