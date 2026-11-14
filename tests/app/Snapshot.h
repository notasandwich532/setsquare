#pragma once

// With SETSQUARE_SNAPSHOT_DIR set, saves a picture of a widget named after the running test,
// for eyeballing rendering changes.

#include <catch2/interfaces/catch_interfaces_capture.hpp>

#include <QDir>
#include <QRegularExpression>
#include <QWidget>

inline void saveSnapshot(QWidget& widget, const QString& suffix = {})
{
    const QString dir = qEnvironmentVariable("SETSQUARE_SNAPSHOT_DIR");
    if (dir.isEmpty()) {
        return;
    }
    QString name = QString::fromStdString(Catch::getResultCapture().getCurrentTestName()) + suffix;
    name.replace(QRegularExpression("[^A-Za-z0-9]+"), "_");
    widget.grab().save(QDir(dir).filePath(name + ".png"));
}
