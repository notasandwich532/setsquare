#pragma once

#include <core/Sketch.h>

#include <QWidget>

#include <utility>
#include <vector>

class Document;
class QListWidget;
class SketchView;

// Lists the sketch's constraints, dimensions and features (patterns, offsets). Selection is
// shared with the canvas; double-click edits a dimension or feature, Delete removes.
class ConstraintPanel : public QWidget
{
    Q_OBJECT

public:
    ConstraintPanel(Document& doc, SketchView& view, QWidget* parent = nullptr);

private:
    void rebuild();
    void pushSelectionToView();
    void pullSelectionFromView();
    QString describe(const core::Constraint& c) const;

    Document& m_doc;
    SketchView& m_view;
    QListWidget* m_list;
    bool m_syncing = false;

    // What the list currently shows, to skip rebuilding while geometry is merely dragged.
    struct Shown
    {
        core::Id id;
        double value;
        std::string expression;
        std::vector<double> params;
        bool conflicting;
        bool redundant;
        bool operator==(const Shown&) const = default;
    };
    std::vector<Shown> m_shown;
};
