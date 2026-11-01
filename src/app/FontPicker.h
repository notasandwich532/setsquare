#pragma once

#include <QWidget>

class QLineEdit;
class QListWidget;

// Choosing a font: a search box over the installed families, each listed in its own typeface.
class FontPicker : public QWidget
{
    Q_OBJECT

public:
    explicit FontPicker(QWidget* parent = nullptr);

    QString family() const { return m_family; }
    void setFamily(const QString& family);

signals:
    void familyChanged(const QString& family);

private:
    void refill();  // the list, for what's in the search box

    QLineEdit* m_search;
    QListWidget* m_list;
    QString m_family;
};
