#pragma once

#include <core/Geometry.h>

#include <QPointF>

class QPainter;
class SketchView;

// On-canvas manipulators that commands let you drag: arrows for distances, a rotation knob, and
// a square for free moving. Positions are in world units; sizes are fixed on screen.
struct Handle
{
    enum class Kind
    {
        Arrow,   // points along `dir` from `at`
        Rotate,  // a round knob at `at`
        Move,    // a square at `at`
    };
    Kind kind = Kind::Arrow;
    int id = 0;
    core::Vec2 at;
    core::Vec2 dir{1.0, 0.0};  // arrows: unit direction (world)
};

void drawHandle(const SketchView& view, QPainter& painter, const Handle& handle, bool hot);
bool hitHandle(const SketchView& view, const Handle& handle, QPointF screen);
