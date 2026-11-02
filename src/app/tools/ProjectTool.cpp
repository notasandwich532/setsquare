#include "ProjectTool.h"

#include "../Document.h"
#include "../SketchView.h"

#include <core/Features.h>
#include <core/Geometry.h>

using core::Id;
using core::kNoId;

namespace
{
constexpr double kPickPixels = 7.0;
}  // namespace

QString ProjectTool::hint() const
{
    const core::Sketch& s = doc().sketch();
    if (s.layers().size() < 2) {
        return QObject::tr("Project: there's only one layer; add another in the Browser to project from it");
    }
    if (m_hover != kNoId) {
        const Id layer = s.layerOf(m_hover);
        return QObject::tr("Project: click to bring this into %1 (from %2); double-click for its whole chain")
            .arg(QString::fromStdString(s.layer(s.activeLayer()).name), QString::fromStdString(s.layer(layer).name));
    }
    return QObject::tr("Project: click geometry in another layer to bring a linked copy into this one "
                       "(double-click: a whole chain). Esc when done");
}

Id ProjectTool::pickAt(QPointF screen) const
{
    const core::Sketch& s = doc().sketch();
    core::PickOptions options;
    options.tolerance = kPickPixels / view().zoom();
    options.otherLayers = s.activeLayer();
    return core::pick(s, view().toWorld(screen), options);
}

void ProjectTool::move(const ToolEvent& e)
{
    m_hover = pickAt(e.screen);
    view().setHovered(m_hover);
    refresh();
}

void ProjectTool::project(const std::vector<Id>& sources)
{
    const core::Sketch& s = doc().sketch();
    std::vector<Id> todo;
    for (Id id : sources) {
        const bool already = std::ranges::any_of(s.entities(), [&](const auto& kv) {
            return kv.second.projectedFrom == id && s.layerOf(kv.first) == s.activeLayer();
        });
        if (!already) {
            todo.push_back(id);
        }
    }
    if (todo.empty()) {
        emit view().message(QObject::tr("That's projected into this layer already"));
        return;
    }
    view().tryEdit(QObject::tr("Project"), [&](Editor& ed) { ed.project(todo); });
}

void ProjectTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Id hit = pickAt(e.screen);
    if (hit != kNoId) {
        project({hit});
    }
    refresh();
}

void ProjectTool::doubleClick(const ToolEvent& e)
{
    const Id hit = pickAt(e.screen);
    if (hit == kNoId || doc().sketch().entity(hit).kind() == core::EntityKind::Point) {
        return;
    }
    try {
        project(core::feature::chainAt(doc().sketch(), hit));
    }
    catch (const core::SketchError&) {
    }
    refresh();
}
