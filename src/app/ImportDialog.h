#pragma once

#include "Import.h"

#include <QDialog>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;

// File > Import's options, for an SVG or DXF file: where it goes (the sublayer being edited, a new
// sublayer, a new layer), whether the file's layers stay apart, where it's placed, how its curves
// come in, and (if the file leaves them open) its units. Shows what's in the file as it reads it.
class ImportDialog : public QDialog
{
    Q_OBJECT

public:
    // `sublayer`: the name of the one being edited; `hasPage`: whether the drawing has a page.
    ImportDialog(const QString& path, const QString& sublayer, bool hasPage, QWidget* parent = nullptr);

    bool readable() const { return m_readable; }
    QString error() const { return m_error; }

    importer::Options options() const;  // (Placement::Center: `at` is the caller's to set)
    double pxPerInch() const;
    std::optional<double> mmPerUnit() const;

private:
    void reread();  // the file again, with the units as set: what's in it

    QString m_path;
    bool m_svg = false;
    bool m_readable = false;
    QString m_error;
    QLabel* m_summary = nullptr;
    QComboBox* m_into = nullptr;
    QCheckBox* m_keepLayers = nullptr;
    QCheckBox* m_usePage = nullptr;  // (an SVG with a page: it becomes the sketch's)
    bool m_sketchHasPage = false;
    QComboBox* m_placement = nullptr;
    QComboBox* m_curves = nullptr;
    QComboBox* m_pxPerInch = nullptr;
    QComboBox* m_units = nullptr;
};
