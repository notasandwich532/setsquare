#pragma once

#include "Tool.h"

#include <core/Sketch.h>

#include <optional>
#include <vector>

// Applies one kind of geometric constraint (or Fix). Works either way round, like Fusion:
// pick the tool then click geometry, or select geometry first and then pick the tool.
// Constraints that would over-constrain the sketch are rejected with a message.
class ConstraintTool : public Tool
{
public:
    ConstraintTool(SketchView& view, ToolKind kind);

    QString hint() const override;
    void activate() override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;

private:
    enum class Outcome
    {
        Incomplete,  // needs more picks
        Applied,
        Rejected,  // complete but refused (message shown)
    };
    Outcome tryApply(const std::vector<core::Id>& refs);
    size_t maxPicks() const;
    // A selection of several things constrained together: each line (horizontal/vertical), or
    // the first paired with each of the others (equal, parallel, coincident, ...).
    std::vector<std::vector<core::Id>> groupsFor(const std::vector<core::Id>& items) const;
    bool applyToMany(const std::vector<core::Id>& items);  // false if it doesn't apply

    ToolKind m_kind;
    std::vector<core::Id> m_picks;
};

// Smart dimension (Fusion's "D"): click one or two entities, move the mouse to choose the
// placement (which also picks horizontal / vertical / aligned for lengths), click to place,
// then type the value.
class DimensionTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

    struct Candidate
    {
        core::ConstraintType type;
        std::vector<core::Id> refs;
        std::uint8_t flip = 0;  // angles: which of the four (from where the label goes)
    };
    // The dimension the picks would make with the label at `mouse`; nullopt if they don't make
    // one (yet). `incomplete` is set when more picks could still make one.
    static std::optional<Candidate> candidateFor(const core::Sketch& sketch, const std::vector<core::Id>& picks,
                                                 core::Vec2 mouse, bool* incomplete = nullptr);

private:
    void reset();

    std::vector<core::Id> m_picks;
    core::Vec2 m_mouse;
};
