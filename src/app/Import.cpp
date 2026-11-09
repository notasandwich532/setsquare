#include "Import.h"

#include "Editor.h"
#include "Images.h"
#include "TextShapes.h"

#include <core/Curves.h>
#include <core/Export.h>
#include <core/Groups.h>
#include <core/Hatch.h>
#include <core/Regions.h>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QUrl>
#include <QHash>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <set>
#include <unordered_map>

namespace importer
{

using core::Color;
using core::Id;
using core::kNoId;
using core::Vec2;

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2 * kPi;

Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 mul(Vec2 a, double k) { return {a.x * k, a.y * k}; }
Vec2 lerp(Vec2 a, Vec2 b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double len(Vec2 a) { return std::hypot(a.x, a.y); }

// x' = a x + c y + e, y' = b x + d y + f (SVG's matrix(a b c d e f)).
struct Affine
{
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Vec2 operator()(Vec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Vec2 linear(Vec2 p) const { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
    double det() const { return a * d - b * c; }
    // This after `inner`.
    Affine operator*(const Affine& i) const
    {
        return {a * i.a + c * i.b, b * i.a + d * i.b, a * i.c + c * i.d, b * i.c + d * i.d,
                a * i.e + c * i.f + e, b * i.e + d * i.f + f};
    }
    static Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    static Affine rotate(double radians)
    {
        const double c = std::cos(radians), s = std::sin(radians);
        return {c, s, -s, c, 0, 0};
    }
};

Vec2 ellipseAt(const Segment& s, double t)
{
    const double cp = std::cos(s.phi), sp = std::sin(s.phi);
    const double x = s.rx * std::cos(t), y = s.ry * std::sin(t);
    return {s.c.x + x * cp - y * sp, s.c.y + x * sp + y * cp};
}

Segment line(Vec2 a, Vec2 b)
{
    Segment s;
    s.kind = Segment::Kind::Line;
    s.p[0] = a;
    s.p[1] = b;
    return s;
}

Segment cubic(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    Segment s;
    s.kind = Segment::Kind::Cubic;
    s.p[0] = a;
    s.p[1] = b;
    s.p[2] = c;
    s.p[3] = d;
    return s;
}

Segment ellipse(Vec2 c, double rx, double ry, double phi, double t0, double dt)
{
    Segment s;
    s.kind = Segment::Kind::Ellipse;
    s.c = c;
    s.rx = rx;
    s.ry = ry;
    s.phi = phi;
    s.t0 = t0;
    s.dt = dt;
    return s;
}

// A segment under an affine map (an ellipse stays an ellipse: its new axes are found).
Segment transformed(const Segment& s, const Affine& m)
{
    Segment out = s;
    if (s.kind != Segment::Kind::Ellipse) {
        for (Vec2& p : out.p) {
            p = m(p);
        }
        return out;
    }
    out.c = m(s.c);
    const Vec2 m1 = m.linear({s.rx * std::cos(s.phi), s.rx * std::sin(s.phi)});
    const Vec2 m2 = m.linear({-s.ry * std::sin(s.phi), s.ry * std::cos(s.phi)});
    // P(t) = c + m1 cos t + m2 sin t; from t* on, its axes are at right angles.
    const double tStar = 0.5 * std::atan2(2 * dot(m1, m2), dot(m1, m1) - dot(m2, m2));
    const Vec2 a1 = add(mul(m1, std::cos(tStar)), mul(m2, std::sin(tStar)));
    const Vec2 a2 = add(mul(m1, -std::sin(tStar)), mul(m2, std::cos(tStar)));
    const double sigma = cross(a1, a2) >= 0 ? 1.0 : -1.0;
    out.rx = len(a1);
    out.ry = len(a2);
    out.phi = std::atan2(a1.y, a1.x);
    out.t0 = sigma * (s.t0 - tStar);
    out.dt = sigma * s.dt;
    return out;
}

// A picture shown over the parallelogram from `corner` (its bottom-left, as shown) along `right`
// (its bottom edge) and `up` (its left edge), in drawing coordinates. A reference image keeps its
// own proportions (as wide as `right`): false if it's shown stretched or skewed.
bool placeImage(ImageDef& def, Vec2 corner, Vec2 right, Vec2 up)
{
    core::Image& im = def.image;
    const Vec2 centre = add(corner, mul(add(right, up), 0.5));
    im.x = centre.x;
    im.y = centre.y;
    im.width = len(right);
    if (cross(right, up) >= 0) {
        im.angle = std::atan2(right.y, right.x) * 180 / kPi;
    }
    else {
        // Mirrored: turned by its left edge, flipped left to right.
        im.angle = std::atan2(up.y, up.x) * 180 / kPi - 90;
        im.flipH = true;
    }
    def.corners = {corner, add(corner, right), add(corner, add(right, up)), add(corner, up)};
    const double lr = len(right), lu = len(up);
    return lr > 0 && lu > 0 && std::abs(dot(right, up)) <= 1e-6 * lr * lu && std::abs(lu - im.height()) <= 1e-3 * lu;
}

// The first of the fonts asked for that's installed (CSS's generic families too: serif, monospace,
// ...); else the one new text starts with, and `substituted` is set.
QString installedFamily(const QStringList& wanted, bool& substituted)
{
    static const QStringList installed = QFontDatabase::families();
    for (QString name : wanted) {
        name = name.trimmed();
        name.remove(QLatin1Char('"')).remove(QLatin1Char('\''));
        if (name.isEmpty()) {
            continue;
        }
        for (const QString& f : installed) {
            if (f.compare(name, Qt::CaseInsensitive) == 0) {
                return f;
            }
        }
        static const QHash<QString, QFont::StyleHint> generic = {
            {QStringLiteral("serif"), QFont::Serif},          {QStringLiteral("sans-serif"), QFont::SansSerif},
            {QStringLiteral("monospace"), QFont::Monospace},  {QStringLiteral("cursive"), QFont::Cursive},
            {QStringLiteral("fantasy"), QFont::Fantasy},      {QStringLiteral("system-ui"), QFont::System},
        };
        if (const auto it = generic.find(name.toLower()); it != generic.end()) {
            QFont f;
            f.setStyleHint(it.value());
            f.setFamily(QString());
            return QFontInfo(f).family();
        }
    }
    substituted = true;
    return textshapes::defaultFamily();
}

// A font's capital height over its size (what text heights here are of, against CSS's font-size).
double capitalRatio(const QString& family, bool bold, bool italic)
{
    QFont f(family);
    f.setPixelSize(1000);
    f.setBold(bold);
    f.setItalic(italic);
    const QFontMetricsF fm(f);
    const double cap = fm.capHeight() > 0 ? fm.capHeight() : fm.ascent() * 0.7;
    return std::clamp(cap / 1000.0, 0.3, 1.0);
}

// Turns a text to run along `right` with its letters standing along `up` (drawing coordinates);
// mirrored if those turn the other way: flipped in place, its alignment swapped to stay put.
void orientText(core::Text& t, Vec2 right, Vec2 up)
{
    if (cross(right, up) >= 0) {
        t.angle = std::atan2(right.y, right.x) * 180 / kPi;
        return;
    }
    t.angle = std::atan2(up.y, up.x) * 180 / kPi - 90;
    t.flipH = true;
    if (t.align != core::TextAlign::Center) {
        t.align = t.align == core::TextAlign::Left ? core::TextAlign::Right : core::TextAlign::Left;
    }
}

// A DXF string: UTF-8 (2007 on), else the old ANSI code page; its %%-codes and \U+ escapes.
QString dxfString(const std::string& raw)
{
    QString s = QString::fromUtf8(raw);
    if (s.contains(QChar::ReplacementCharacter)) {
        s = QString::fromLatin1(raw);
    }
    static const QRegularExpression unicode(QStringLiteral("\\\\U\\+([0-9A-Fa-f]{4})"));
    for (auto m = unicode.match(s); m.hasMatch(); m = unicode.match(s)) {
        s.replace(m.capturedStart(), m.capturedLength(), QChar(static_cast<char16_t>(m.captured(1).toUInt(nullptr, 16))));
    }
    s.replace(QStringLiteral("%%c"), QStringLiteral("⌀"), Qt::CaseInsensitive);
    s.replace(QStringLiteral("%%d"), QStringLiteral("°"), Qt::CaseInsensitive);
    s.replace(QStringLiteral("%%p"), QStringLiteral("±"), Qt::CaseInsensitive);
    static const QRegularExpression toggles(QStringLiteral("%%[uUoOkK]"));  // (underline, overline, strike: not kept)
    s.remove(toggles);
    s.replace(QStringLiteral("%%%"), QStringLiteral("%"));
    return s;
}

// MTEXT's words without their formatting codes (\P a new line; fonts, heights, colours dropped;
// stacked fractions as a/b). `font`: the first font it switches to, if any.
QString mtextPlain(const QString& in, QString& font)
{
    QString out;
    for (qsizetype i = 0; i < in.size(); ++i) {
        const QChar c = in[i];
        if (c == QLatin1Char('{') || c == QLatin1Char('}')) {
            continue;  // (grouping)
        }
        if (c != QLatin1Char('\\') || i + 1 >= in.size()) {
            out += c;
            continue;
        }
        const QChar code = in[++i];
        switch (code.unicode()) {
            case 'P': out += QLatin1Char('\n'); break;
            case '~': out += QChar(0x00a0); break;
            case '\\': case '{': case '}': out += code; break;
            case 'L': case 'l': case 'O': case 'o': case 'K': case 'k': case 'N': break;  // (toggles)
            case 'S': {  // a stacked fraction up to ';': "1^2" or "1/2" or "1#2"
                const qsizetype end = in.indexOf(QLatin1Char(';'), i);
                QString frac = in.mid(i + 1, (end < 0 ? in.size() : end) - i - 1);
                frac.replace(QLatin1Char('^'), QLatin1Char('/')).replace(QLatin1Char('#'), QLatin1Char('/'));
                out += frac.trimmed();
                i = end < 0 ? in.size() : end;
                break;
            }
            case 'f': case 'F': {  // a font: "\fArial|b0|i0;"
                const qsizetype end = in.indexOf(QLatin1Char(';'), i);
                if (font.isEmpty()) {
                    font = in.mid(i + 1, (end < 0 ? in.size() : end) - i - 1).section(QLatin1Char('|'), 0, 0);
                }
                i = end < 0 ? in.size() : end;
                break;
            }
            default: {  // others with a value up to ';' (\H, \C, \T, \Q, \W, \A, \p, ...)
                const qsizetype end = in.indexOf(QLatin1Char(';'), i);
                i = end < 0 ? in.size() : end;
                break;
            }
        }
    }
    return out;
}

// A linked picture: by its path as given (absolute, or from the file's folder), else by its name
// in that folder. Empty if it isn't found.
QString findLinked(const QString& given, const QString& folder)
{
    QString path = given;
    if (path.startsWith(QStringLiteral("file://"))) {
        path = QUrl(path).toLocalFile();
    }
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo as(QDir(folder).filePath(path));
    if (as.isFile()) {
        return as.absoluteFilePath();
    }
    const QString name = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/')).section(QLatin1Char('/'), -1);
    const QFileInfo byName(QDir(folder).filePath(name));
    return !folder.isEmpty() && byName.isFile() ? byName.absoluteFilePath() : QString();
}

// --- Cutting outlines (clip paths, pattern tiles) ---------------------------------------------

// A segment as one of the sketch's curve paths (to find where it crosses others).
core::curve::Path pathOf(const Segment& s)
{
    switch (s.kind) {
        case Segment::Kind::Line: return core::curve::Path::line(s.p[0], s.p[1]);
        case Segment::Kind::Cubic:
            return core::curve::Path::spline({core::curve::Bezier{{s.p[0], s.p[1], s.p[2], s.p[3]}}}, false, 0.0, 1.0);
        case Segment::Kind::Ellipse: break;
    }
    core::curve::EllipseFrame f;
    f.c = s.c;
    f.u = {std::cos(s.phi), std::sin(s.phi)};
    f.a = s.rx;
    f.b = s.ry;
    if (s.whole()) {
        return core::curve::Path::ellipse(f);
    }
    return s.dt > 0 ? core::curve::Path::ellipseArc(f, s.t0, s.dt) : core::curve::Path::ellipseArc(f, s.t0 + s.dt, -s.dt);
}

// How far along a segment (0 to 1) a point on it is.
double fractionAlong(const Segment& s, const core::curve::Path& p, Vec2 q)
{
    const double t = p.param(q);
    if (s.kind != Segment::Kind::Ellipse) {
        return std::clamp(t, 0.0, 1.0);
    }
    if (s.whole()) {
        double f = std::fmod((s.dt > 0 ? t - s.t0 : s.t0 - t) / kTwoPi, 1.0);
        return f < 0 ? f + 1.0 : f;
    }
    const double f = std::clamp(t / std::abs(s.dt), 0.0, 1.0);
    return s.dt > 0 ? f : 1.0 - f;
}

// The same curve between two fractions along it.
Segment pieceOf(const Segment& s, double f0, double f1)
{
    switch (s.kind) {
        case Segment::Kind::Line: return line(lerp(s.p[0], s.p[1], f0), lerp(s.p[0], s.p[1], f1));
        case Segment::Kind::Cubic: {
            const core::curve::Bezier part = core::curve::Bezier{{s.p[0], s.p[1], s.p[2], s.p[3]}}.part(f0, f1);
            return cubic(part.p[0], part.p[1], part.p[2], part.p[3]);
        }
        case Segment::Kind::Ellipse: break;
    }
    return ellipse(s.c, s.rx, s.ry, s.phi, s.t0 + s.dt * f0, s.dt * (f1 - f0));
}

std::vector<std::vector<Vec2>> ringsOf(const std::vector<Contour>& contours)
{
    std::vector<std::vector<Vec2>> rings;
    for (const Contour& c : contours) {
        std::vector<Vec2> ring;
        for (const Segment& s : c.segments) {
            const std::vector<Vec2> pts = s.sample(12);
            ring.insert(ring.end(), pts.begin(), pts.end());
        }
        if (ring.size() >= 3) {
            rings.push_back(std::move(ring));  // (open ones close themselves, as SVG fills them)
        }
    }
    return rings;
}

bool pointInShape(const std::vector<std::vector<Vec2>>& rings, Vec2 p, bool evenOdd);

bool inArea(const Area& a, Vec2 p)
{
    return pointInShape(a.rings, p, a.evenOdd);
}

// Segments with what they need for finding crossings quickly.
struct Cuttable
{
    const Segment* segment;
    core::curve::Path path;
    std::vector<Vec2> samples;
    double x0, y0, x1, y1;
};

std::vector<Cuttable> cuttable(const std::vector<const Segment*>& segments)
{
    std::vector<Cuttable> out;
    for (const Segment* s : segments) {
        Cuttable c{s, pathOf(*s), {}, 1e300, 1e300, -1e300, -1e300};
        c.samples = core::curve::crossingSamples(c.path);
        for (const Vec2& p : s->sample(16)) {
            c.x0 = std::min(c.x0, p.x);
            c.y0 = std::min(c.y0, p.y);
            c.x1 = std::max(c.x1, p.x);
            c.y1 = std::max(c.y1, p.y);
        }
        const double pad = 1e-6 * std::max({1.0, c.x1 - c.x0, c.y1 - c.y0});
        c.x0 -= pad;
        c.y0 -= pad;
        c.x1 += pad;
        c.y1 += pad;
        out.push_back(std::move(c));
    }
    return out;
}

// The pieces of a segment between where it crosses the others, each kept if `keep` says so of
// its middle.
template <typename Keep>
void cutSegment(const Cuttable& s, const std::vector<Cuttable>& by, const Keep& keep, std::vector<Segment>& out)
{
    std::vector<double> cuts{0.0, 1.0};
    for (const Cuttable& o : by) {
        if (o.x0 > s.x1 || o.x1 < s.x0 || o.y0 > s.y1 || o.y1 < s.y0) {
            continue;
        }
        for (const Vec2& q : core::curve::intersections(s.path, s.samples, o.path, o.samples)) {
            cuts.push_back(fractionAlong(*s.segment, s.path, q));
        }
    }
    std::ranges::sort(cuts);
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        if (cuts[i + 1] - cuts[i] < 1e-9) {
            continue;
        }
        const double middle = (cuts[i] + cuts[i + 1]) / 2;
        if (keep(pieceOf(*s.segment, cuts[i], middle).end())) {
            out.push_back(pieceOf(*s.segment, cuts[i], cuts[i + 1]));
        }
    }
}

// A shape's outlines cut to a clip (inside any of its areas). A filled one also gets the parts of
// the clip's outline that are inside it (`insideShape`), with no line of their own, so its fill
// closes where it's cut.
std::vector<Contour> cutTo(const std::vector<Contour>& contours, const std::vector<Area>& clip, bool filled,
                           const std::function<bool(Vec2)>& insideShape)
{
    auto inClip = [&](Vec2 p) { return std::ranges::any_of(clip, [&](const Area& a) { return inArea(a, p); }); };
    std::vector<const Segment*> shapeSegments, clipSegments;
    for (const Contour& c : contours) {
        for (const Segment& s : c.segments) {
            shapeSegments.push_back(&s);
        }
    }
    for (const Area& a : clip) {
        for (const Contour& c : a.contours) {
            for (const Segment& s : c.segments) {
                clipSegments.push_back(&s);
            }
        }
    }
    const std::vector<Cuttable> shapeCut = cuttable(shapeSegments), clipCut = cuttable(clipSegments);
    std::vector<Contour> out;
    size_t k = 0;
    for (const Contour& c : contours) {
        Contour kept;
        kept.lineless = c.lineless;
        for (size_t i = 0; i < c.segments.size(); ++i, ++k) {
            cutSegment(shapeCut[k], clipCut, inClip, kept.segments);
        }
        if (!kept.segments.empty()) {
            out.push_back(std::move(kept));
        }
    }
    if (filled) {
        // (each area's outline where it's inside the shape, and not inside another of the clip's areas)
        size_t j = 0;
        for (size_t a = 0; a < clip.size(); ++a) {
            for (const Contour& c : clip[a].contours) {
                Contour edge;
                edge.lineless = true;
                for (size_t i = 0; i < c.segments.size(); ++i, ++j) {
                    cutSegment(clipCut[j], shapeCut, [&](Vec2 p) {
                        if (!insideShape(p)) {
                            return false;
                        }
                        for (size_t b = 0; b < clip.size(); ++b) {
                            if (b != a && inArea(clip[b], p)) {
                                return false;
                            }
                        }
                        return true;
                    }, edge.segments);
                }
                if (!edge.segments.empty()) {
                    out.push_back(std::move(edge));
                }
            }
        }
    }
    return out;
}

// A segment that's really straight (a line, or a cubic with its handles on the line).
bool straightSegment(const Segment& s)
{
    if (s.kind == Segment::Kind::Line) {
        return true;
    }
    if (s.kind != Segment::Kind::Cubic) {
        return false;
    }
    const Vec2 d = sub(s.p[3], s.p[0]);
    const double l = len(d);
    return l > 1e-12 && std::abs(cross(d, sub(s.p[1], s.p[0]))) / l < 1e-9 * std::max(1.0, l)
        && std::abs(cross(d, sub(s.p[2], s.p[0]))) / l < 1e-9 * std::max(1.0, l);
}

// Which way a segment runs at its start and at its end.
Vec2 startDirection(const Segment& s)
{
    switch (s.kind) {
        case Segment::Kind::Line: return sub(s.p[1], s.p[0]);
        case Segment::Kind::Cubic:
            for (int k = 1; k <= 3; ++k) {
                if (len(sub(s.p[k], s.p[0])) > 1e-12) {
                    return sub(s.p[k], s.p[0]);
                }
            }
            return {1, 0};
        case Segment::Kind::Ellipse: break;
    }
    const double t = s.t0, sign = s.dt < 0 ? -1.0 : 1.0;
    const Vec2 d{-s.rx * std::sin(t) * sign, s.ry * std::cos(t) * sign};
    return {d.x * std::cos(s.phi) - d.y * std::sin(s.phi), d.x * std::sin(s.phi) + d.y * std::cos(s.phi)};
}

Vec2 endDirection(const Segment& s)
{
    switch (s.kind) {
        case Segment::Kind::Line: return sub(s.p[1], s.p[0]);
        case Segment::Kind::Cubic:
            for (int k = 2; k >= 0; --k) {
                if (len(sub(s.p[3], s.p[k])) > 1e-12) {
                    return sub(s.p[3], s.p[k]);
                }
            }
            return {1, 0};
        case Segment::Kind::Ellipse: break;
    }
    const double t = s.t0 + s.dt, sign = s.dt < 0 ? -1.0 : 1.0;
    const Vec2 d{-s.rx * std::sin(t) * sign, s.ry * std::cos(t) * sign};
    return {d.x * std::cos(s.phi) - d.y * std::sin(s.phi), d.x * std::sin(s.phi) + d.y * std::cos(s.phi)};
}

bool pointInShape(const std::vector<std::vector<Vec2>>& rings, Vec2 p, bool evenOdd)
{
    int winding = 0, crossings = 0;
    for (const auto& ring : rings) {
        for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
            const Vec2 a = ring[j], b = ring[i];
            if ((a.y <= p.y) != (b.y <= p.y)) {
                const double x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (x > p.x) {
                    ++crossings;
                    winding += b.y > a.y ? 1 : -1;
                }
            }
        }
    }
    return evenOdd ? (crossings % 2) == 1 : winding != 0;
}

// Centripetal Catmull-Rom pieces through points (an approximation as Beziers).
std::vector<Segment> throughPoints(const std::vector<Vec2>& pts, bool closed)
{
    std::vector<Segment> out;
    const size_t n = pts.size();
    if (n < 2) {
        return out;
    }
    auto P = [&](long i) {
        const long m = static_cast<long>(n);
        return pts[static_cast<size_t>(((i % m) + m) % m)];
    };
    const long pieces = closed ? static_cast<long>(n) : static_cast<long>(n) - 1;
    for (long i = 0; i < pieces; ++i) {
        const Vec2 p1 = P(i), p2 = P(i + 1);
        const Vec2 p0 = !closed && i == 0 ? sub(mul(p1, 2), p2) : P(i - 1);
        const Vec2 p3 = !closed && i == pieces - 1 ? sub(mul(p2, 2), p1) : P(i + 2);
        auto d = [](Vec2 a, Vec2 b) { return std::max(std::sqrt(len(sub(b, a))), 1e-9); };
        const double d1 = d(p0, p1), d2 = d(p1, p2), d3 = d(p2, p3);
        const Vec2 b1 = mul(add(sub(mul(p2, d1 * d1), mul(p0, d2 * d2)), mul(p1, 2 * d1 * d1 + 3 * d1 * d2 + d2 * d2)),
                            1.0 / (3 * d1 * (d1 + d2)));
        const Vec2 b2 = mul(add(sub(mul(p1, d3 * d3), mul(p3, d2 * d2)), mul(p2, 2 * d3 * d3 + 3 * d3 * d2 + d2 * d2)),
                            1.0 / (3 * d3 * (d3 + d2)));
        out.push_back(cubic(p1, b1, b2, p2));
    }
    return out;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Segments and drawings
// ---------------------------------------------------------------------------------------------

bool Segment::whole() const
{
    return kind == Kind::Ellipse && std::abs(dt) >= kTwoPi - 1e-9;
}

Vec2 Segment::start() const
{
    return kind == Kind::Ellipse ? ellipseAt(*this, t0) : p[0];
}

Vec2 Segment::end() const
{
    switch (kind) {
        case Kind::Line: return p[1];
        case Kind::Cubic: return p[3];
        case Kind::Ellipse: return ellipseAt(*this, t0 + dt);
    }
    return p[0];
}

std::vector<Vec2> Segment::sample(int perPiece) const
{
    std::vector<Vec2> out;
    switch (kind) {
        case Kind::Line: out = {p[0], p[1]}; break;
        case Kind::Cubic:
            for (int i = 0; i <= perPiece; ++i) {
                const double t = double(i) / perPiece, u = 1 - t;
                const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
                out.push_back({b0 * p[0].x + b1 * p[1].x + b2 * p[2].x + b3 * p[3].x,
                               b0 * p[0].y + b1 * p[1].y + b2 * p[2].y + b3 * p[3].y});
            }
            break;
        case Kind::Ellipse: {
            const int n = std::max(perPiece, static_cast<int>(std::ceil(std::abs(dt) / (kPi / 32))));
            for (int i = 0; i <= n; ++i) {
                out.push_back(ellipseAt(*this, t0 + dt * i / n));
            }
            break;
        }
    }
    return out;
}

bool Drawing::bounds(double& x0, double& y0, double& x1, double& y1) const
{
    x0 = y0 = 1e300;
    x1 = y1 = -1e300;
    for (const TextDef& td : texts) {  // (its anchor: the letters aren't made yet)
        x0 = std::min(x0, td.at.x);
        y0 = std::min(y0, td.at.y);
        x1 = std::max(x1, td.at.x);
        y1 = std::max(y1, td.at.y);
    }
    for (const ImageDef& im : images) {
        for (const Vec2& p : im.corners) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    }
    for (const Shape& s : shapes) {
        for (const Contour& c : s.contours) {
            for (const Segment& seg : c.segments) {
                for (const Vec2& p : seg.sample(8)) {
                    x0 = std::min(x0, p.x);
                    y0 = std::min(y0, p.y);
                    x1 = std::max(x1, p.x);
                    y1 = std::max(y1, p.y);
                }
            }
        }
    }
    return x0 <= x1;
}

bool GradientDef::operator==(const GradientDef& o) const
{
    auto same = [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; };
    return kind == o.kind && spread == o.spread && stops == o.stops && same(start, o.start) && same(end, o.end)
        && same(center, o.center) && rx == o.rx && ry == o.ry && phi == o.phi && focal.has_value() == o.focal.has_value()
        && (!focal || same(*focal, *o.focal));
}

QString Report::summary() const
{
    auto counted = [](int n, const QString& one, const QString& many) { return n == 1 ? one : many.arg(n); };
    QStringList parts;
    parts << counted(curves, QObject::tr("1 curve"), QObject::tr("%1 curves"));
    if (fills > 0) {
        parts << counted(fills, QObject::tr("1 fill"), QObject::tr("%1 fills"));
    }
    if (texts > 0) {
        parts << counted(texts, QObject::tr("1 text"), QObject::tr("%1 texts"));
    }
    if (images > 0) {
        parts << counted(images, QObject::tr("1 image"), QObject::tr("%1 images"));
    }
    if (hatches > 0) {
        parts << counted(hatches, QObject::tr("1 hatch"), QObject::tr("%1 hatches"));
    }
    if (gradients > 0) {
        parts << counted(gradients, QObject::tr("1 gradient"), QObject::tr("%1 gradients"));
    }
    QString text = parts.join(QStringLiteral(", "));
    if (!sublayers.empty()) {
        text += QStringLiteral(" ") + counted(static_cast<int>(sublayers.size()), QObject::tr("in 1 sublayer"),
                                            QObject::tr("in %1 sublayers"));
    }
    if (!skipped.empty()) {
        QStringList what;
        for (const auto& [kind, count] : skipped) {
            what << QStringLiteral("%1 %2").arg(count).arg(QString::fromStdString(kind));
        }
        text += QObject::tr("; skipped %1").arg(what.join(QStringLiteral(", ")));
    }
    return text;
}

// =============================================================================================
// SVG
// =============================================================================================

namespace
{
struct Node
{
    QString tag;
    QHash<QString, QString> attrs;
    std::vector<Node> kids;
    QString text;
};

Node parseXml(const QByteArray& data)
{
    QXmlStreamReader xml(data);
    Node root;
    std::vector<Node*> stack{&root};
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            Node n;
            n.tag = xml.name().toString();
            for (const QXmlStreamAttribute& a : xml.attributes()) {
                n.attrs.insert(a.qualifiedName().toString(), a.value().toString());
                if (a.qualifiedName() != a.name()) {
                    n.attrs.insert(a.name().toString(), a.value().toString());  // (xlink:href as href too)
                }
            }
            stack.back()->kids.push_back(std::move(n));
            stack.push_back(&stack.back()->kids.back());
        }
        else if (xml.isEndElement()) {
            if (stack.size() > 1) {
                stack.pop_back();
            }
        }
        else if (xml.isCharacters() || xml.isCDATA()) {
            Node& parent = *stack.back();
            parent.text += xml.text();
            // In text, words and the tspans between them in order: as children of their own.
            if (parent.tag == QStringLiteral("text") || parent.tag == QStringLiteral("tspan")
                || parent.tag == QStringLiteral("textPath") || parent.tag == QStringLiteral("a")) {
                Node words;
                words.tag = QStringLiteral("#text");
                words.text = xml.text().toString();
                parent.kids.push_back(std::move(words));
            }
        }
    }
    if (xml.hasError() && root.kids.empty()) {
        throw core::SketchError("not an SVG file (" + xml.errorString().toStdString() + ")");
    }
    if (root.kids.empty() || root.kids.front().tag != QStringLiteral("svg")) {
        throw core::SketchError("not an SVG file (no <svg> element)");
    }
    return std::move(root.kids.front());
}

// The CSS colour names.
const QHash<QString, QString>& namedColors()
{
    static const QHash<QString, QString> names = [] {
        const char* table =
            "aliceblue f0f8ff antiquewhite faebd7 aqua 00ffff aquamarine 7fffd4 azure f0ffff beige f5f5dc bisque ffe4c4 "
            "black 000000 blanchedalmond ffebcd blue 0000ff blueviolet 8a2be2 brown a52a2a burlywood deb887 cadetblue "
            "5f9ea0 chartreuse 7fff00 chocolate d2691e coral ff7f50 cornflowerblue 6495ed cornsilk fff8dc crimson dc143c "
            "cyan 00ffff darkblue 00008b darkcyan 008b8b darkgoldenrod b8860b darkgray a9a9a9 darkgreen 006400 darkgrey "
            "a9a9a9 darkkhaki bdb76b darkmagenta 8b008b darkolivegreen 556b2f darkorange ff8c00 darkorchid 9932cc darkred "
            "8b0000 darksalmon e9967a darkseagreen 8fbc8f darkslateblue 483d8b darkslategray 2f4f4f darkslategrey 2f4f4f "
            "darkturquoise 00ced1 darkviolet 9400d3 deeppink ff1493 deepskyblue 00bfff dimgray 696969 dimgrey 696969 "
            "dodgerblue 1e90ff firebrick b22222 floralwhite fffaf0 forestgreen 228b22 fuchsia ff00ff gainsboro dcdcdc "
            "ghostwhite f8f8ff gold ffd700 goldenrod daa520 gray 808080 grey 808080 green 008000 greenyellow adff2f "
            "honeydew f0fff0 hotpink ff69b4 indianred cd5c5c indigo 4b0082 ivory fffff0 khaki f0e68c lavender e6e6fa "
            "lavenderblush fff0f5 lawngreen 7cfc00 lemonchiffon fffacd lightblue add8e6 lightcoral f08080 lightcyan "
            "e0ffff lightgoldenrodyellow fafad2 lightgray d3d3d3 lightgreen 90ee90 lightgrey d3d3d3 lightpink ffb6c1 "
            "lightsalmon ffa07a lightseagreen 20b2aa lightskyblue 87cefa lightslategray 778899 lightslategrey 778899 "
            "lightsteelblue b0c4de lightyellow ffffe0 lime 00ff00 limegreen 32cd32 linen faf0e6 magenta ff00ff maroon "
            "800000 mediumaquamarine 66cdaa mediumblue 0000cd mediumorchid ba55d3 mediumpurple 9370db mediumseagreen "
            "3cb371 mediumslateblue 7b68ee mediumspringgreen 00fa9a mediumturquoise 48d1cc mediumvioletred c71585 "
            "midnightblue 191970 mintcream f5fffa mistyrose ffe4e1 moccasin ffe4b5 navajowhite ffdead navy 000080 "
            "oldlace fdf5e6 olive 808000 olivedrab 6b8e23 orange ffa500 orangered ff4500 orchid da70d6 palegoldenrod "
            "eee8aa palegreen 98fb98 paleturquoise afeeee palevioletred db7093 papayawhip ffefd5 peachpuff ffdab9 peru "
            "cd853f pink ffc0cb plum dda0dd powderblue b0e0e6 purple 800080 rebeccapurple 663399 red ff0000 rosybrown "
            "bc8f8f royalblue 4169e1 saddlebrown 8b4513 salmon fa8072 sandybrown f4a460 seagreen 2e8b57 seashell fff5ee "
            "sienna a0522d silver c0c0c0 skyblue 87ceeb slateblue 6a5acd slategray 708090 slategrey 708090 snow fffafa "
            "springgreen 00ff7f steelblue 4682b4 tan d2b48c teal 008080 thistle d8bfd8 tomato ff6347 turquoise 40e0d0 "
            "violet ee82ee wheat f5deb3 white ffffff whitesmoke f5f5f5 yellow ffff00 yellowgreen 9acd32";
        QHash<QString, QString> out;
        const QStringList words = QString::fromLatin1(table).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (qsizetype i = 0; i + 1 < words.size(); i += 2) {
            out.insert(words[i], words[i + 1]);
        }
        return out;
    }();
    return names;
}

double firstOr(const std::vector<double>& v, double fallback)
{
    return v.empty() ? fallback : v.front();
}

// Numbers in a list (with SVG's packed forms: "1.5.5", "1-2", exponents).
std::vector<double> numbers(const QString& text)
{
    std::vector<double> out;
    static const QRegularExpression number(QStringLiteral("[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?"));
    auto it = number.globalMatch(text);
    while (it.hasNext()) {
        out.push_back(it.next().captured().toDouble());
    }
    return out;
}

class SvgReader
{
public:
    SvgReader(const Node& root, double pxPerInch, const QString& folder)
        : m_root(root)
        , m_pxPerInch(pxPerInch)
        , m_folder(folder)
    {
        index(root);
        for (const Node* style : m_styles) {
            parseCss(style->text);
        }
    }

    Drawing read()
    {
        // Size: width/height (any units) over the viewBox; without units, pixels.
        const double pxMm = 25.4 / m_pxPerInch;
        const std::vector<double> vb = numbers(m_root.attrs.value(QStringLiteral("viewBox")));
        const std::optional<double> w = length(m_root.attrs.value(QStringLiteral("width")), pxMm);
        const std::optional<double> h = length(m_root.attrs.value(QStringLiteral("height")), pxMm);
        double sx = pxMm, sy = pxMm, minX = 0, minY = 0, tx = 0, ty = 0;
        std::optional<double> W = w, H = h;
        if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) {
            minX = vb[0];
            minY = vb[1];
            if (w && h) {
                sx = *w / vb[2];
                sy = *h / vb[3];
                if (!m_root.attrs.value(QStringLiteral("preserveAspectRatio")).startsWith(QStringLiteral("none"))) {
                    const double s = std::min(sx, sy);  // (fitted in, centred)
                    tx = (*w - vb[2] * s) / 2;
                    ty = (*h - vb[3] * s) / 2;
                    sx = sy = s;
                }
            }
            else if (w) {
                sx = sy = *w / vb[2];
                H = vb[3] * sy;
            }
            else if (h) {
                sx = sy = *h / vb[3];
                W = vb[2] * sx;
            }
            else {
                W = vb[2] * sx;
                H = vb[3] * sy;
            }
        }
        const bool unitsGiven = m_root.attrs.value(QStringLiteral("width")).contains(QRegularExpression(QStringLiteral("[a-z]")));
        m_out.units = unitsGiven ? "from the file's size" : "pixels, " + std::to_string(int(std::lround(m_pxPerInch))) + " to the inch";
        // To drawing coordinates: y up, the page's bottom-left corner on the origin.
        const double height = H.value_or(0.0);
        const Affine base{sx, 0, 0, -sy, -sx * minX + tx, height + sy * minY - ty};
        if (W && H) {
            m_out.canvas = std::array<double, 4>{0.0, 0.0, *W, *H};
        }
        // The viewport in user units (what userSpaceOnUse percentages are of).
        m_viewport = vb.size() == 4 && vb[2] > 0 && vb[3] > 0 ? Vec2{vb[2], vb[3]}
                                                              : Vec2{W.value_or(0.0) / pxMm, H.value_or(0.0) / pxMm};
        Paint paint;
        walk(m_root, base, paint, 1.0, std::string(), 0, true);
        for (const std::string& layer : m_layerOrder) {
            m_out.layers.push_back(layer);
        }
        return std::move(m_out);
    }

private:
    struct Paint  // the inherited style
    {
        QString fill = QStringLiteral("black");
        QString stroke = QStringLiteral("none");
        double strokeWidth = 1.0;  // user units
        QString dash = QStringLiteral("none");
        double fillOpacity = 1.0;
        double strokeOpacity = 1.0;
        QString fillRule = QStringLiteral("nonzero");
        QString color = QStringLiteral("black");
        bool visible = true;
        QString fontFamily = QStringLiteral("sans-serif");
        double fontSize = 16.0;  // user units
        QString fontWeight = QStringLiteral("normal");
        QString fontStyle = QStringLiteral("normal");
        QString textAnchor = QStringLiteral("start");
        double letterSpacing = 0.0;  // user units
        QString markerStart, markerMid, markerEnd;  // url(#id)s, or empty
    };

    void index(const Node& n)
    {
        if (n.attrs.contains(QStringLiteral("id"))) {
            m_byId.insert(n.attrs.value(QStringLiteral("id")), &n);
        }
        if (n.tag == QStringLiteral("style")) {
            m_styles.push_back(&n);
        }
        for (const Node& k : n.kids) {
            index(k);
        }
    }

    // Simple CSS: rules of element, .class, #id and element.class selectors.
    void parseCss(QString text)
    {
        static const QRegularExpression comments(QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption);
        text.remove(comments);
        static const QRegularExpression rule(QStringLiteral("([^{}]+)\\{([^}]*)\\}"));
        auto it = rule.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            const QHash<QString, QString> decls = declarations(m.captured(2));
            for (QString selector : m.captured(1).split(QLatin1Char(','))) {
                selector = selector.trimmed();
                if (!selector.isEmpty() && !selector.contains(QLatin1Char(' ')) && !selector.contains(QLatin1Char('>'))) {
                    m_css.push_back({selector, decls});
                }
            }
        }
    }

    static QHash<QString, QString> declarations(const QString& text)
    {
        QHash<QString, QString> out;
        for (const QString& part : text.split(QLatin1Char(';'))) {
            const qsizetype colon = part.indexOf(QLatin1Char(':'));
            if (colon > 0) {
                out.insert(part.left(colon).trimmed(), part.mid(colon + 1).trimmed());
            }
        }
        return out;
    }

    // An element's own style properties: presentation attributes, then CSS, then style="".
    QHash<QString, QString> ownStyle(const Node& n) const
    {
        QHash<QString, QString> props;
        static const QStringList names = {
            QStringLiteral("fill"),          QStringLiteral("stroke"),         QStringLiteral("stroke-width"),
            QStringLiteral("stroke-dasharray"), QStringLiteral("fill-opacity"), QStringLiteral("stroke-opacity"),
            QStringLiteral("opacity"),       QStringLiteral("fill-rule"),      QStringLiteral("display"),
            QStringLiteral("visibility"),    QStringLiteral("color"),          QStringLiteral("clip-path"),
            QStringLiteral("mask"),          QStringLiteral("font-family"),    QStringLiteral("font-size"),
            QStringLiteral("font-weight"),   QStringLiteral("font-style"),     QStringLiteral("text-anchor"),
            QStringLiteral("letter-spacing"), QStringLiteral("font"),           QStringLiteral("marker"),
            QStringLiteral("marker-start"),  QStringLiteral("marker-mid"),     QStringLiteral("marker-end"),
        };
        for (const QString& name : names) {
            if (n.attrs.contains(name)) {
                props.insert(name, n.attrs.value(name));
            }
        }
        const QStringList classes = n.attrs.value(QStringLiteral("class")).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString id = n.attrs.value(QStringLiteral("id"));
        for (const auto& [selector, decls] : m_css) {
            bool matches = selector == QStringLiteral("*") || selector == n.tag
                           || (!id.isEmpty() && selector == QLatin1Char('#') + id);
            for (const QString& c : classes) {
                matches = matches || selector == QLatin1Char('.') + c || selector == n.tag + QLatin1Char('.') + c;
            }
            if (matches) {
                for (auto it = decls.begin(); it != decls.end(); ++it) {
                    props.insert(it.key(), it.value());
                }
            }
        }
        const QHash<QString, QString> inline_ = declarations(n.attrs.value(QStringLiteral("style")));
        for (auto it = inline_.begin(); it != inline_.end(); ++it) {
            props.insert(it.key(), it.value());
        }
        return props;
    }

    std::optional<double> length(const QString& text, double pxMm) const
    {
        static const QRegularExpression re(QStringLiteral("^\\s*([-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?)\\s*([a-z%]*)\\s*$"));
        const auto m = re.match(text);
        if (!m.hasMatch()) {
            return std::nullopt;
        }
        const double v = m.captured(1).toDouble();
        const QString unit = m.captured(2);
        if (unit == QStringLiteral("mm")) return v;
        if (unit == QStringLiteral("cm")) return v * 10;
        if (unit == QStringLiteral("in")) return v * 25.4;
        if (unit == QStringLiteral("pt")) return v * 25.4 / 72;
        if (unit == QStringLiteral("pc")) return v * 25.4 / 6;
        if (unit == QStringLiteral("%")) return std::nullopt;
        return v * pxMm;  // px, or none
    }

    double number(const Node& n, const char* name, double fallback = 0.0) const
    {
        const std::vector<double> v = numbers(n.attrs.value(QLatin1String(name)));
        return v.empty() ? fallback : v.front();
    }

    Affine transformOf(const QString& text) const
    {
        Affine m;
        static const QRegularExpression fn(QStringLiteral("(matrix|translate|scale|rotate|skewX|skewY)\\s*\\(([^)]*)\\)"));
        auto it = fn.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            const QString name = match.captured(1);
            const std::vector<double> v = numbers(match.captured(2));
            Affine t;
            if (name == QStringLiteral("matrix") && v.size() == 6) {
                t = {v[0], v[1], v[2], v[3], v[4], v[5]};
            }
            else if (name == QStringLiteral("translate") && !v.empty()) {
                t = Affine::translate(v[0], v.size() > 1 ? v[1] : 0.0);
            }
            else if (name == QStringLiteral("scale") && !v.empty()) {
                t = Affine::scale(v[0], v.size() > 1 ? v[1] : v[0]);
            }
            else if (name == QStringLiteral("rotate") && !v.empty()) {
                t = Affine::rotate(v[0] * kPi / 180);
                if (v.size() == 3) {
                    t = Affine::translate(v[1], v[2]) * t * Affine::translate(-v[1], -v[2]);
                }
            }
            else if (name == QStringLiteral("skewX") && !v.empty()) {
                t = {1, 0, std::tan(v[0] * kPi / 180), 1, 0, 0};
            }
            else if (name == QStringLiteral("skewY") && !v.empty()) {
                t = {1, std::tan(v[0] * kPi / 180), 0, 1, 0, 0};
            }
            m = m * t;
        }
        return m;
    }

    std::optional<Color> color(QString text, const Paint& paint, double opacity)
    {
        text = text.trimmed();
        if (text.isEmpty() || text == QStringLiteral("none") || text == QStringLiteral("transparent")) {
            return std::nullopt;
        }
        if (text == QStringLiteral("currentColor")) {
            text = paint.color;
        }
        Color c;
        bool ok = false;
        if (text.startsWith(QStringLiteral("url("))) {
            // A gradient or pattern: its average colour (gradients come in as plain colours).
            static const QRegularExpression ref(QStringLiteral("url\\(\\s*['\"]?#([^)'\"]+)"));
            const auto m = ref.match(text);
            const Node* g = m.hasMatch() ? m_byId.value(m.captured(1)) : nullptr;
            std::vector<Color> stops;
            for (int hops = 0; g && stops.empty() && hops < 8; ++hops) {
                for (const Node& s : g->kids) {
                    if (s.tag == QStringLiteral("stop")) {
                        const QHash<QString, QString> st = ownStyle(s);
                        Paint plain;
                        if (const auto sc = color(st.value(QStringLiteral("stop-color"), s.attrs.value(QStringLiteral("stop-color"), QStringLiteral("black"))), plain, 1.0)) {
                            stops.push_back(*sc);
                        }
                    }
                }
                const QString href = g->attrs.value(QStringLiteral("href"));
                g = href.startsWith(QLatin1Char('#')) ? m_byId.value(href.mid(1)) : nullptr;
            }
            if (stops.empty()) {
                return std::nullopt;
            }
            double r = 0, gg = 0, b = 0, a = 0;
            for (const Color& s : stops) {
                r += s.r;
                gg += s.g;
                b += s.b;
                a += s.a;
            }
            const double n = double(stops.size());
            auto avg = [n](double v) { return static_cast<std::uint8_t>(std::lround(v / n)); };
            c = {avg(r), avg(gg), avg(b), avg(a)};
            ok = true;
        }
        else if (text.startsWith(QLatin1Char('#'))) {
            QString hex = text.mid(1);
            if (hex.size() == 3 || hex.size() == 4) {
                QString wide;
                for (QChar ch : hex) {
                    wide += ch;
                    wide += ch;
                }
                hex = wide;
            }
            ok = Color::fromHex(hex.toStdString(), c);
        }
        else if (text.startsWith(QStringLiteral("rgb"))) {
            const qsizetype open = text.indexOf(QLatin1Char('('));
            const QStringList parts = text.mid(open + 1).remove(QLatin1Char(')')).split(QRegularExpression(QStringLiteral("[,\\s/]+")), Qt::SkipEmptyParts);
            if (parts.size() >= 3) {
                auto channel = [](const QString& p) {
                    const double v = p.endsWith(QLatin1Char('%')) ? p.chopped(1).toDouble() * 2.55 : p.toDouble();
                    return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
                };
                c = {channel(parts[0]), channel(parts[1]), channel(parts[2]), 255};
                if (parts.size() >= 4) {
                    const double a = parts[3].endsWith(QLatin1Char('%')) ? parts[3].chopped(1).toDouble() / 100 : parts[3].toDouble();
                    c.a = static_cast<std::uint8_t>(std::clamp(std::lround(a * 255), 0L, 255L));
                }
                ok = true;
            }
        }
        else if (const auto it = namedColors().find(text.toLower()); it != namedColors().end()) {
            ok = Color::fromHex(it.value().toStdString(), c);
        }
        if (!ok) {
            c = {0, 0, 0, 255};
        }
        c.a = static_cast<std::uint8_t>(std::clamp(std::lround(c.a * opacity), 0L, 255L));
        return c;
    }

    void note(const std::string& text)
    {
        if (std::ranges::find(m_out.notes, text) == m_out.notes.end()) {
            m_out.notes.push_back(text);
        }
    }

    void skip(const std::string& what) { ++m_out.skipped[what]; }

    // The gradient a paint ("url(#id)") refers to, placed for a shape: `outline` in the shape's own
    // user space (its bounding box is what objectBoundingBox units are of), `m` from there to the
    // drawing. None if it isn't a gradient, has fewer than two stops, or can't be placed (a
    // degenerate box): the shape then keeps the gradient's average colour.
    std::optional<GradientDef> gradientFor(const QString& paintText, const Affine& m, const std::vector<Contour>& outline,
                                           double opacity)
    {
        static const QRegularExpression ref(QStringLiteral("^\\s*url\\(\\s*['\"]?#([^)'\"]+)"));
        const auto match = ref.match(paintText);
        const Node* g = match.hasMatch() ? m_byId.value(match.captured(1)) : nullptr;
        if (!g || (g->tag != QStringLiteral("linearGradient") && g->tag != QStringLiteral("radialGradient"))) {
            return std::nullopt;
        }
        const bool radial = g->tag == QStringLiteral("radialGradient");
        // Its chain of templates (href): attributes and stops come from the nearest that has them.
        std::vector<const Node*> chain{g};
        for (int hops = 0; hops < 8; ++hops) {
            const QString href = chain.back()->attrs.value(QStringLiteral("href"));
            const Node* next = href.startsWith(QLatin1Char('#')) ? m_byId.value(href.mid(1)) : nullptr;
            if (!next || std::ranges::find(chain, next) != chain.end()) {
                break;
            }
            chain.push_back(next);
        }
        auto attr = [&](const char* name) -> std::optional<QString> {
            for (const Node* n : chain) {
                if (n->attrs.contains(QLatin1String(name))) {
                    return n->attrs.value(QLatin1String(name));
                }
            }
            return std::nullopt;
        };

        // Stops.
        std::vector<core::GradientStop> stops;
        for (const Node* n : chain) {
            for (const Node& s : n->kids) {
                if (s.tag != QStringLiteral("stop")) {
                    continue;
                }
                const QHash<QString, QString> st = ownStyle(s);
                auto prop = [&](const char* name, const char* fallback) {
                    return st.value(QLatin1String(name), s.attrs.value(QLatin1String(name), QLatin1String(fallback)));
                };
                const QString offsetText = s.attrs.value(QStringLiteral("offset"), QStringLiteral("0")).trimmed();
                double offset = firstOr(numbers(offsetText), 0.0) / (offsetText.endsWith(QLatin1Char('%')) ? 100.0 : 1.0);
                offset = std::clamp(offset, 0.0, 1.0);
                if (!stops.empty()) {
                    offset = std::max(offset, stops.back().offset);  // (never back past the one before)
                }
                Paint plain;
                plain.color = prop("color", "black");
                const double stopOpacity = std::clamp(firstOr(numbers(prop("stop-opacity", "1")), 1.0), 0.0, 1.0);
                if (const auto c = color(prop("stop-color", "black"), plain, stopOpacity * opacity)) {
                    stops.push_back({offset, *c});
                }
                else {
                    stops.push_back({offset, Color{0, 0, 0, 0}});  // ("none": transparent)
                }
            }
            if (!stops.empty()) {
                break;
            }
        }
        if (stops.size() < 2) {
            return std::nullopt;
        }

        // From the gradient's own space to the drawing.
        const bool boxUnits = attr("gradientUnits").value_or(QStringLiteral("objectBoundingBox")) != QStringLiteral("userSpaceOnUse");
        Affine box;
        if (boxUnits) {
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (const Contour& c : outline) {
                for (const Segment& seg : c.segments) {
                    for (const Vec2& p : seg.sample(8)) {
                        x0 = std::min(x0, p.x);
                        y0 = std::min(y0, p.y);
                        x1 = std::max(x1, p.x);
                        y1 = std::max(y1, p.y);
                    }
                }
            }
            if (!(x1 - x0 > 1e-12 && y1 - y0 > 1e-12)) {
                return std::nullopt;  // (SVG doesn't paint it either)
            }
            box = Affine::translate(x0, y0) * Affine::scale(x1 - x0, y1 - y0);
        }
        const Affine t = m * box * transformOf(attr("gradientTransform").value_or(QString()));
        if (std::abs(t.det()) < 1e-18) {
            return std::nullopt;
        }
        // A coordinate: a fraction of the box (box units), or user units (percentages: of the viewport).
        auto coord = [&](const char* name, const char* fallback, bool horizontal) {
            const QString text = attr(name).value_or(QLatin1String(fallback)).trimmed();
            const double v = firstOr(numbers(text), 0.0);
            if (text.endsWith(QLatin1Char('%'))) {
                return v / 100.0 * (boxUnits ? 1.0 : horizontal ? m_viewport.x : m_viewport.y);
            }
            return v;
        };

        GradientDef d;
        d.stops = std::move(stops);
        const QString spread = attr("spreadMethod").value_or(QStringLiteral("pad"));
        d.spread = spread == QStringLiteral("reflect") ? core::GradientSpread::Reflect
                 : spread == QStringLiteral("repeat")  ? core::GradientSpread::Repeat
                                                       : core::GradientSpread::Pad;
        if (!radial) {
            d.kind = core::GradientKind::Linear;
            const Vec2 s{coord("x1", "0%", true), coord("y1", "0%", false)};
            const Vec2 e{coord("x2", "100%", true), coord("y2", "0%", false)};
            const Vec2 dir = sub(e, s);
            const double l2 = dot(dir, dir);
            if (l2 < 1e-24) {
                return std::nullopt;
            }
            // The colour changes along `dir` in the gradient's space; mapped to the drawing, it
            // changes fastest along w = (inverse transpose of t) dir / |dir|^2: the end is where
            // the offset reaches 1 going that way (exact, even under a skew or uneven scale).
            const double det = t.det();
            const Vec2 w{(t.d * dir.x - t.b * dir.y) / det / l2, (-t.c * dir.x + t.a * dir.y) / det / l2};
            const double w2 = dot(w, w);
            if (w2 < 1e-24) {
                return std::nullopt;
            }
            d.start = t(s);
            d.end = add(d.start, mul(w, 1.0 / w2));
            return d;
        }
        d.kind = core::GradientKind::Radial;
        const double cx = coord("cx", "50%", true), cy = coord("cy", "50%", false);
        const double r = coord("r", "50%", true);
        if (!(r > 0)) {
            return std::nullopt;
        }
        const Segment ring = transformed(ellipse({cx, cy}, r, r, 0, 0, kTwoPi), t);
        d.center = ring.c;
        d.rx = ring.rx;
        d.ry = ring.ry;
        d.phi = ring.phi;
        const Vec2 focal{attr("fx") ? coord("fx", "0", true) : cx, attr("fy") ? coord("fy", "0", false) : cy};
        if (len(sub(focal, {cx, cy})) > 1e-9 * r) {
            d.focal = t(focal);
        }
        if (attr("fr") && firstOr(numbers(*attr("fr")), 0.0) > 0) {
            note("radial gradients' focal radius (fr) was left out");
        }
        return d;
    }

    // A picture: held in the file (a data: URI) or linked to. Fitted into its box as SVG does
    // (preserveAspectRatio), then placed by the transform.
    void image(const Node& n, const Affine& m, const std::string& layer, double opacity)
    {
        const QString href = n.attrs.value(QStringLiteral("href")).trimmed();
        QByteArray bytes;
        QString source, name;
        if (href.startsWith(QStringLiteral("data:"))) {
            const qsizetype comma = href.indexOf(QLatin1Char(','));
            if (comma < 0) {
                skip("images that can't be read");
                return;
            }
            const QByteArray payload = href.mid(comma + 1).toLatin1();
            bytes = href.left(comma).contains(QStringLiteral(";base64")) ? QByteArray::fromBase64(payload)
                                                                          : QByteArray::fromPercentEncoding(payload);
            source = QStringLiteral("embedded");
            name = QStringLiteral("an embedded picture");
        }
        else {
            const QString path = findLinked(href, m_folder);
            QFile file(path);
            if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
                skip("linked images not found");
                return;
            }
            bytes = file.readAll();
            source = QDir::toNativeSeparators(path);
            name = QFileInfo(path).fileName();
        }
        ImageDef def;
        try {
            def.image = images::fromBytes(bytes, source, name);
        }
        catch (const core::SketchError&) {
            skip("images that can't be read");
            return;
        }
        const double pw = def.image.pixelWidth, ph = def.image.pixelHeight;
        // Its box (without a size: the picture's own, in pixels: user units).
        const double x = number(n, "x"), y = number(n, "y");
        double w = number(n, "width"), h = number(n, "height");
        if (!(w > 0) && !(h > 0)) {
            w = pw;
            h = ph;
        }
        else if (!(w > 0)) {
            w = h * pw / ph;
        }
        else if (!(h > 0)) {
            h = w * ph / pw;
        }
        // Fitted in (meet) or filling it (slice), aligned; or stretched to it (none).
        const QStringList fit = n.attrs.value(QStringLiteral("preserveAspectRatio"), QStringLiteral("xMidYMid meet"))
                                    .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString align = fit.value(0, QStringLiteral("xMidYMid"));
        bool stretched = false;
        double dw = w, dh = h, ix = x, iy = y;
        if (align == QStringLiteral("none")) {
            stretched = std::abs(w / h - pw / ph) > 1e-6 * (pw / ph);
        }
        else {
            const bool slice = fit.value(1) == QStringLiteral("slice");
            if (slice) {
                note("images cropped to their box (slice) came in whole");
            }
            const double s = slice ? std::max(w / pw, h / ph) : std::min(w / pw, h / ph);
            dw = pw * s;
            dh = ph * s;
            const double ax = align.contains(QStringLiteral("xMin")) ? 0.0 : align.contains(QStringLiteral("xMax")) ? 1.0 : 0.5;
            const double ay = align.contains(QStringLiteral("YMin")) ? 0.0 : align.contains(QStringLiteral("YMax")) ? 1.0 : 0.5;
            ix = x + (w - dw) * ax;
            iy = y + (h - dh) * ay;
        }
        def.layer = layer;
        def.image.opacity = std::clamp(0.5 * opacity, 0.05, 1.0);  // (a reference: faded, as when inserted)
        if (!placeImage(def, m({ix, iy + dh}), m.linear({dw, 0}), m.linear({0, -dh})) || stretched) {
            note("stretched or skewed images came in keeping their own proportions");
        }
        if (!layer.empty() && std::ranges::find(m_layerOrder, layer) == m_layerOrder.end()) {
            m_layerOrder.push_back(layer);
        }
        m_out.images.push_back(std::move(def));
    }

    // A clip-path's areas in drawing coordinates, for an element (`m`: its user space to the
    // drawing). Each of its shapes is an area, by its clip-rule; inside the clip: inside any of
    // them. None if it isn't a clip path (or can't be placed: its units a group's box).
    std::optional<std::vector<Area>> clipAreas(const QString& value, const Affine& m, const Node& element)
    {
        static const QRegularExpression ref(QStringLiteral("url\\(\\s*['\"]?#([^)'\"]+)"));
        const auto match = ref.match(value);
        const Node* clip = match.hasMatch() ? m_byId.value(match.captured(1)) : nullptr;
        if (!clip || clip->tag != QStringLiteral("clipPath")) {
            return std::nullopt;
        }
        Affine base = m * transformOf(clip->attrs.value(QStringLiteral("transform")));
        if (clip->attrs.value(QStringLiteral("clipPathUnits")) == QStringLiteral("objectBoundingBox")) {
            // (in the element's own box: a shape's; a group's isn't known yet)
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (const Contour& c : shapeContours(element)) {
                for (const Segment& s : c.segments) {
                    for (const Vec2& p : s.sample(8)) {
                        x0 = std::min(x0, p.x);
                        y0 = std::min(y0, p.y);
                        x1 = std::max(x1, p.x);
                        y1 = std::max(y1, p.y);
                    }
                }
            }
            if (!(x1 > x0 && y1 > y0)) {
                skip("clip paths sized to a group (drawn unclipped)");
                return std::nullopt;
            }
            base = base * Affine::translate(x0, y0) * Affine::scale(x1 - x0, y1 - y0);
        }
        const QString rule = ownStyle(*clip).value(QStringLiteral("clip-rule"), QStringLiteral("nonzero"));
        std::vector<Area> areas;
        for (const Node& k : clip->kids) {
            const QHash<QString, QString> style = ownStyle(k);
            if (style.value(QStringLiteral("display")) == QStringLiteral("none")) {
                continue;
            }
            Affine at = base * transformOf(k.attrs.value(QStringLiteral("transform")));
            const Node* shape = &k;
            if (k.tag == QStringLiteral("use")) {
                const QString href = k.attrs.value(QStringLiteral("href"));
                shape = href.startsWith(QLatin1Char('#')) ? m_byId.value(href.mid(1)) : nullptr;
                if (!shape) {
                    continue;
                }
                at = at * Affine::translate(number(k, "x"), number(k, "y")) * transformOf(shape->attrs.value(QStringLiteral("transform")));
            }
            else if (k.tag == QStringLiteral("text")) {
                skip("clip paths made of text (drawn unclipped)");
                return std::nullopt;
            }
            Area a;
            a.contours = shapeContours(*shape);
            for (Contour& c : a.contours) {
                c.closed = true;  // (an open one clips as if closed)
                for (Segment& s : c.segments) {
                    s = transformed(s, at);
                }
            }
            a.rings = ringsOf(a.contours);
            a.evenOdd = style.value(QStringLiteral("clip-rule"), rule) == QStringLiteral("evenodd");
            if (!a.rings.empty()) {
                areas.push_back(std::move(a));
            }
        }
        return areas;  // (none: nothing shows through)
    }

    // A width or height in user units; 0 if it's missing or a percentage (then it isn't known here).
    double size(const Node& n, const char* name) const
    {
        const QString text = n.attrs.value(QLatin1String(name)).trimmed();
        return text.isEmpty() || text.endsWith(QLatin1Char('%')) ? 0.0 : fontLength(text, 16.0);
    }

    // A viewBox mapped into a w x h viewport (aligned and fitted in, or filling it, or stretched:
    // preserveAspectRatio). Without a size, its own units (just moved to its corner).
    Affine viewBoxMap(const Node& n, double w, double h) const
    {
        const std::vector<double> vb = numbers(n.attrs.value(QStringLiteral("viewBox")));
        if (vb.size() != 4 || !(vb[2] > 0) || !(vb[3] > 0)) {
            return {};
        }
        if (!(w > 0)) {
            w = h > 0 ? h * vb[2] / vb[3] : vb[2];
        }
        if (!(h > 0)) {
            h = w * vb[3] / vb[2];
        }
        double sx = w / vb[2], sy = h / vb[3], tx = 0, ty = 0;
        const QStringList fit = n.attrs.value(QStringLiteral("preserveAspectRatio"), QStringLiteral("xMidYMid meet"))
                                    .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString align = fit.value(0, QStringLiteral("xMidYMid"));
        if (align != QStringLiteral("none")) {
            const double s = fit.value(1) == QStringLiteral("slice") ? std::max(sx, sy) : std::min(sx, sy);
            const double ax = align.contains(QStringLiteral("xMin")) ? 0.0 : align.contains(QStringLiteral("xMax")) ? 1.0 : 0.5;
            const double ay = align.contains(QStringLiteral("YMin")) ? 0.0 : align.contains(QStringLiteral("YMax")) ? 1.0 : 0.5;
            sx = sy = s;
            tx = (w - vb[2] * s) * ax;
            ty = (h - vb[3] * s) * ay;
        }
        return Affine::translate(tx, ty) * Affine::scale(sx, sy) * Affine::translate(-vb[0], -vb[1]);
    }

    // An element's own style over what it inherits (the properties that are inherited).
    void inherit(Paint& paint, const QHash<QString, QString>& own) const
    {
        // The font shorthand first ("italic bold 12px/1.5 Georgia, serif"), the longhands over it.
        if (own.contains(QStringLiteral("font"))) {
            static const QRegularExpression shorthand(QStringLiteral(
                "^\\s*((?:(?:italic|oblique|normal|bold|bolder|lighter|small-caps|[1-9]00)\\s+)*)"
                "([\\d.]+(?:px|pt|mm|cm|in|em|%)?)(?:\\s*/\\s*\\S+)?\\s+(.+)$"));
            const auto m = shorthand.match(own.value(QStringLiteral("font")));
            if (m.hasMatch()) {
                const QStringList words = m.captured(1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
                for (const QString& w : words) {
                    if (w == QStringLiteral("italic") || w == QStringLiteral("oblique")) {
                        paint.fontStyle = w;
                    }
                    else if (w == QStringLiteral("bold") || w == QStringLiteral("bolder") || w.endsWith(QStringLiteral("00"))) {
                        paint.fontWeight = w;
                    }
                }
                paint.fontSize = fontLength(m.captured(2), paint.fontSize);
                paint.fontFamily = m.captured(3).trimmed();
            }
        }
        auto set = [&](const char* name, QString& field) {
            if (own.contains(QLatin1String(name)) && own.value(QLatin1String(name)) != QStringLiteral("inherit")) {
                field = own.value(QLatin1String(name));
            }
        };
        set("fill", paint.fill);
        set("stroke", paint.stroke);
        set("stroke-dasharray", paint.dash);
        set("fill-rule", paint.fillRule);
        set("color", paint.color);
        set("font-family", paint.fontFamily);
        set("font-weight", paint.fontWeight);
        set("font-style", paint.fontStyle);
        set("text-anchor", paint.textAnchor);
        if (own.contains(QStringLiteral("marker"))) {  // (all three at once)
            paint.markerStart = paint.markerMid = paint.markerEnd = own.value(QStringLiteral("marker"));
        }
        set("marker-start", paint.markerStart);
        set("marker-mid", paint.markerMid);
        set("marker-end", paint.markerEnd);
        if (own.contains(QStringLiteral("stroke-width"))) {
            const std::vector<double> v = numbers(own.value(QStringLiteral("stroke-width")));
            if (!v.empty()) {
                paint.strokeWidth = v.front() * (own.value(QStringLiteral("stroke-width")).endsWith(QStringLiteral("mm")) ? m_pxPerInch / 25.4 : 1.0);
            }
        }
        if (own.contains(QStringLiteral("fill-opacity"))) {
            paint.fillOpacity = firstOr(numbers(own.value(QStringLiteral("fill-opacity"))), 1.0);
        }
        if (own.contains(QStringLiteral("stroke-opacity"))) {
            paint.strokeOpacity = firstOr(numbers(own.value(QStringLiteral("stroke-opacity"))), 1.0);
        }
        if (own.contains(QStringLiteral("visibility"))) {
            paint.visible = own.value(QStringLiteral("visibility")) == QStringLiteral("visible");
        }
        // Sizes: in user units (pixels), or relative to the parent's (em, %).
        if (own.contains(QStringLiteral("font-size"))) {
            paint.fontSize = fontLength(own.value(QStringLiteral("font-size")), paint.fontSize);
        }
        if (own.contains(QStringLiteral("letter-spacing"))) {
            const QString ls = own.value(QStringLiteral("letter-spacing"));
            paint.letterSpacing = ls == QStringLiteral("normal") ? 0.0 : fontLength(ls, paint.fontSize);
        }
    }

    // A font size or spacing in user units: px (or none), pt, mm, ...; em and % of `em`.
    double fontLength(const QString& text, double em) const
    {
        static const QHash<QString, double> named = {
            {QStringLiteral("xx-small"), 9}, {QStringLiteral("x-small"), 10}, {QStringLiteral("small"), 13},
            {QStringLiteral("medium"), 16},  {QStringLiteral("large"), 18},   {QStringLiteral("x-large"), 24},
            {QStringLiteral("xx-large"), 32},
        };
        const QString t = text.trimmed();
        if (const auto it = named.find(t); it != named.end()) {
            return it.value();
        }
        const double v = firstOr(numbers(t), 0.0);
        if (t.endsWith(QStringLiteral("em"))) return v * em;
        if (t.endsWith(QLatin1Char('%'))) return v / 100 * em;
        if (t.endsWith(QStringLiteral("pt"))) return v * 96.0 / 72.0;
        if (t.endsWith(QStringLiteral("mm"))) return v * m_pxPerInch / 25.4;
        if (t.endsWith(QStringLiteral("cm"))) return v * m_pxPerInch / 2.54;
        if (t.endsWith(QStringLiteral("in"))) return v * m_pxPerInch;
        return v;
    }

    // Text: a run of words for each place the text (or a tspan in it) is put, each a text here.
    void text(const Node& n, const Affine& m, const Paint& paint, double opacity, const std::string& layer)
    {
        std::function<bool(const Node&)> alongPath = [&](const Node& k) {
            return k.tag == QStringLiteral("textPath") || std::ranges::any_of(k.kids, alongPath);
        };
        if (alongPath(n)) {
            textOnPath(n, m, paint, opacity, layer);
            return;
        }
        // A run: the words put at one place (the text's, or a placed tspan's: a line), in chunks
        // of one style each.
        struct Chunk
        {
            QString words;
            Paint paint;
        };
        struct Run
        {
            std::vector<Chunk> chunks;
            Vec2 at;  // user units
            Paint paint;  // (the run's own: its text-anchor)
        };
        auto first = [&](const Node& k, const char* name) -> std::optional<double> {
            const std::vector<double> v = numbers(k.attrs.value(QLatin1String(name)));
            return v.empty() ? std::nullopt : std::optional<double>(v.front());
        };
        auto alike = [](const Paint& a, const Paint& b) {
            return a.fill == b.fill && a.stroke == b.stroke && a.fillOpacity == b.fillOpacity && a.fontFamily == b.fontFamily
                && a.fontSize == b.fontSize && a.fontWeight == b.fontWeight && a.fontStyle == b.fontStyle
                && a.letterSpacing == b.letterSpacing && a.visible == b.visible;
        };
        std::vector<Run> runs{{{}, {first(n, "x").value_or(0.0) + first(n, "dx").value_or(0.0),
                                    first(n, "y").value_or(0.0) + first(n, "dy").value_or(0.0)},
                               paint}};
        std::function<void(const Node&, const Paint&, int)> collect = [&](const Node& at, const Paint& p, int depth) {
            for (const Node& k : at.kids) {
                if (k.tag == QStringLiteral("#text")) {
                    QString words = k.text;
                    words.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
                    Run& run = runs.back();
                    if (run.chunks.empty() || !alike(run.chunks.back().paint, p)) {
                        run.chunks.push_back({{}, p});
                    }
                    run.chunks.back().words += words;
                    continue;
                }
                if ((k.tag != QStringLiteral("tspan") && k.tag != QStringLiteral("a")) || depth > 8) {
                    continue;
                }
                const QHash<QString, QString> own = ownStyle(k);
                if (own.value(QStringLiteral("display")) == QStringLiteral("none")) {
                    continue;
                }
                Paint kp = p;
                inherit(kp, own);
                const auto x = first(k, "x"), y = first(k, "y"), dx = first(k, "dx"), dy = first(k, "dy");
                if (x || y || dx || dy) {  // (placed: words of their own, e.g. a line)
                    const Vec2 from = runs.back().at;
                    runs.push_back({{}, {x.value_or(from.x) + dx.value_or(0.0), y.value_or(from.y) + dy.value_or(0.0)}, kp});
                }
                collect(k, kp, depth + 1);
            }
        };
        collect(n, paint, 0);

        for (Run& run : runs) {
            // Spaces collapsed across the chunks (as SVG does), none at the line's ends.
            bool space = true;  // (just after one, or at the start)
            for (Chunk& c : run.chunks) {
                QString out;
                for (const QChar ch : c.words) {
                    if (ch == QLatin1Char(' ')) {
                        if (!space) {
                            out += ch;
                        }
                        space = true;
                    }
                    else {
                        out += ch;
                        space = false;
                    }
                }
                c.words = out;
            }
            if (!run.chunks.empty()) {
                while (run.chunks.back().words.endsWith(QLatin1Char(' '))) {
                    run.chunks.back().words.chop(1);
                }
            }
            std::erase_if(run.chunks, [](const Chunk& c) { return c.words.isEmpty(); });
            if (run.chunks.empty()) {
                continue;
            }
            const QString anchor = run.chunks.front().paint.textAnchor;
            if (run.chunks.size() == 1) {
                if (auto def = textDef(run.chunks.front().words, run.chunks.front().paint, anchor, m, run.at, opacity, layer)) {
                    m_out.texts.push_back(std::move(*def));
                }
                continue;
            }
            // Words in different styles: a text for each, one after another along the line (the
            // line placed by its anchor as a whole).
            std::vector<double> widths;
            double total = 0;
            for (const Chunk& c : run.chunks) {
                widths.push_back(advance(c.words, c.paint));
                total += widths.back();
            }
            double x = run.at.x - (anchor == QStringLiteral("middle") ? total / 2 : anchor == QStringLiteral("end") ? total : 0.0);
            for (size_t i = 0; i < run.chunks.size(); ++i) {
                const Chunk& c = run.chunks[i];
                const qsizetype lead = c.words.size() - QString(c.words).remove(QRegularExpression(QStringLiteral("^ +"))).size();
                const QString words = c.words.trimmed();
                const double before = advance(c.words.left(lead), c.paint);
                if (auto def = textDef(words, c.paint, QStringLiteral("start"), m, {x + before, run.at.y}, opacity, layer)) {
                    m_out.texts.push_back(std::move(*def));
                }
                x += widths[i];
            }
        }
    }

    // Text along a path (<textPath href="#path">): its words set along that path, which comes in
    // as construction curves for it to follow. Placed at its start, middle or end (startOffset 0,
    // 50% or 100% with text-anchor start, middle or end); part way along: at its start (noted).
    void textOnPath(const Node& n, const Affine& m, const Paint& paint, double opacity, const std::string& layer)
    {
        std::function<const Node*(const Node&)> find = [&](const Node& k) -> const Node* {
            if (k.tag == QStringLiteral("textPath")) {
                return &k;
            }
            for (const Node& c : k.kids) {
                if (const Node* f = find(c)) {
                    return f;
                }
            }
            return nullptr;
        };
        const Node* tp = find(n);
        const QString href = tp->attrs.value(QStringLiteral("href"));
        const Node* target = href.startsWith(QLatin1Char('#')) ? m_byId.value(href.mid(1)) : nullptr;
        std::vector<Contour> outline;
        Affine at = m;
        if (target) {
            at = m * transformOf(target->attrs.value(QStringLiteral("transform")));
            outline = shapeContours(*target);
        }
        else if (tp->attrs.contains(QStringLiteral("path"))) {
            outline = pathContours(tp->attrs.value(QStringLiteral("path")));  // (SVG 2: its own path)
        }
        std::erase_if(outline, [](const Contour& c) { return c.segments.empty(); });
        if (outline.empty()) {
            skip("texts along paths that can't be found");
            return;
        }
        if (outline.size() > 1) {
            note("texts along paths of several parts follow the first");
        }
        Contour path = outline.front();
        for (Segment& s : path.segments) {
            s = transformed(s, at);
        }
        // Its words (in one style: the first words').
        Paint style = paint;
        inherit(style, ownStyle(*tp));
        QString words;
        bool styled = false;
        std::function<void(const Node&, const Paint&)> collect = [&](const Node& k, const Paint& p) {
            for (const Node& c : k.kids) {
                if (c.tag == QStringLiteral("#text")) {
                    if (!styled && !c.text.trimmed().isEmpty()) {
                        style = p;
                        styled = true;
                    }
                    words += c.text;
                }
                else if (c.tag == QStringLiteral("tspan") || c.tag == QStringLiteral("a")) {
                    Paint cp = p;
                    inherit(cp, ownStyle(c));
                    collect(c, cp);
                }
            }
        };
        Paint tpPaint = paint;
        inherit(tpPaint, ownStyle(*tp));
        collect(*tp, tpPaint);
        words = words.simplified();
        // Where along it: its start, middle or end.
        const QString offsetText = tp->attrs.value(QStringLiteral("startOffset"), QStringLiteral("0")).trimmed();
        const bool percent = offsetText.endsWith(QLatin1Char('%'));
        const double offset = firstOr(numbers(offsetText), 0.0);
        const QString anchor = style.textAnchor;
        QString placed = QStringLiteral("start");
        if (std::abs(offset) < 1e-9 && anchor == QStringLiteral("start")) {
            placed = QStringLiteral("start");
        }
        else if (percent && std::abs(offset - 50) < 1e-6 && anchor == QStringLiteral("middle")) {
            placed = QStringLiteral("middle");
        }
        else if (percent && std::abs(offset - 100) < 1e-6 && anchor == QStringLiteral("end")) {
            placed = QStringLiteral("end");
        }
        else {
            note("texts placed part way along their paths came in at the path's start");
        }
        auto def = textDef(words, style, placed, at, path.segments.front().start(), opacity, layer);
        if (!def) {
            return;
        }
        def->text.flipH = false;  // (the path sets its way; see build)
        def->text.angle = 0.0;
        def->at = path.segments.front().start();
        def->path = std::move(path);
        m_out.texts.push_back(std::move(*def));
    }

    // How far words run in a style (user units).
    double advance(const QString& words, const Paint& p)
    {
        bool substituted = false;
        QFont font(installedFamily(p.fontFamily.split(QLatin1Char(',')), substituted));
        font.setPixelSize(1000);
        const int weight = p.fontWeight == QStringLiteral("bold") || p.fontWeight == QStringLiteral("bolder") ? 700
                         : static_cast<int>(firstOr(numbers(p.fontWeight), 400));
        font.setBold(weight >= 600);
        font.setItalic(p.fontStyle == QStringLiteral("italic") || p.fontStyle == QStringLiteral("oblique"));
        return QFontMetricsF(font).horizontalAdvance(words) / 1000.0 * p.fontSize + p.letterSpacing * double(words.size());
    }

    // A text of words in a style, anchored at `at` (user units) by `anchor` (start, middle, end).
    std::optional<TextDef> textDef(const QString& words, const Paint& p, const QString& anchor, const Affine& m, Vec2 at,
                                   double opacity, const std::string& layer)
    {
        if (words.isEmpty() || !p.visible) {
            return std::nullopt;
        }
        TextDef def;
        core::Text& t = def.text;
        t.text = words.toStdString();
        bool substituted = false;
        t.font = installedFamily(p.fontFamily.split(QLatin1Char(',')), substituted).toStdString();
        if (substituted) {
            note("fonts that aren't installed were replaced");
        }
        const int weight = p.fontWeight == QStringLiteral("bold") || p.fontWeight == QStringLiteral("bolder") ? 700
                         : static_cast<int>(firstOr(numbers(p.fontWeight), 400));
        t.bold = weight >= 600;
        t.italic = p.fontStyle == QStringLiteral("italic") || p.fontStyle == QStringLiteral("oblique");
        t.align = anchor == QStringLiteral("middle") ? core::TextAlign::Center
                : anchor == QStringLiteral("end")    ? core::TextAlign::Right
                                                     : core::TextAlign::Left;
        // Its size: capitals of the font at that size, scaled as the transform scales up.
        const Vec2 right = m.linear({1, 0}), up = m.linear({0, -1});
        if (std::abs(len(right) - len(up)) > 1e-6 * len(up) || std::abs(dot(right, up)) > 1e-6 * len(right) * len(up)) {
            note("stretched or skewed texts came in unstretched");
        }
        t.height = p.fontSize * len(up) * capitalRatio(QString::fromStdString(t.font), t.bold, t.italic);
        if (!(t.height > 1e-9)) {
            return std::nullopt;
        }
        t.spacing = p.fontSize > 0 ? p.letterSpacing / p.fontSize * 100.0 : 0.0;
        orientText(t, right, up);
        // Filled in its colour (or, outlined only, its line's).
        std::optional<Color> c = color(p.fill, p, p.fillOpacity * opacity);
        if (!c) {
            c = color(p.stroke, p, p.strokeOpacity * opacity);
        }
        if (!c) {
            return std::nullopt;  // (not painted at all)
        }
        t.hasColor = true;
        t.color = *c;
        def.at = m(at);
        def.layer = layer;
        if (!layer.empty() && std::ranges::find(m_layerOrder, layer) == m_layerOrder.end()) {
            m_layerOrder.push_back(layer);
        }
        return def;
    }
    void walk(const Node& n, const Affine& parent, const Paint& inherited, double opacity, std::string layer, int depth,
              bool isRoot = false)
    {
        if (depth > 64) {
            return;
        }
        const QHash<QString, QString> own = ownStyle(n);
        if (own.value(QStringLiteral("display")) == QStringLiteral("none")) {
            return;
        }
        Paint paint = inherited;
        inherit(paint, own);
        if (own.contains(QStringLiteral("opacity"))) {
            opacity *= firstOr(numbers(own.value(QStringLiteral("opacity"))), 1.0);
        }
        if (own.contains(QStringLiteral("mask")) && own.value(QStringLiteral("mask")) != QStringLiteral("none")) {
            skip("masks");
        }
        const Affine m = isRoot ? parent : parent * transformOf(n.attrs.value(QStringLiteral("transform")));
        // A clip: everything in this element is cut to it (on top of any it's in already).
        bool clipped = false;
        if (own.contains(QStringLiteral("clip-path")) && own.value(QStringLiteral("clip-path")) != QStringLiteral("none")) {
            if (auto areas = clipAreas(own.value(QStringLiteral("clip-path")), m, n)) {
                m_clips.push_back(std::move(*areas));
                clipped = true;
            }
        }
        struct Unclip
        {
            std::vector<std::vector<Area>>& clips;
            bool on;
            ~Unclip()
            {
                if (on) {
                    clips.pop_back();
                }
            }
        } unclip{m_clips, clipped};

        const QString& tag = n.tag;
        if (tag == QStringLiteral("g") || tag == QStringLiteral("svg") || tag == QStringLiteral("a")
            || tag == QStringLiteral("switch")) {
            // Inkscape's layers (ours too): top-level ones name a layer; inside them, "Parent/Child".
            if (n.attrs.value(QStringLiteral("inkscape:groupmode")) == QStringLiteral("layer")) {
                const QString label = n.attrs.value(QStringLiteral("inkscape:label"), n.attrs.value(QStringLiteral("id")));
                layer = layer.empty() ? label.toStdString() : layer + "/" + label.toStdString();
            }
            Affine inner = m;
            if (tag == QStringLiteral("svg") && !isRoot) {
                // Its viewBox fitted to its size, there.
                inner = m * Affine::translate(number(n, "x"), number(n, "y")) * viewBoxMap(n, size(n, "width"), size(n, "height"));
            }
            for (const Node& k : n.kids) {
                walk(k, inner, paint, opacity, layer, depth + 1);
            }
            return;
        }
        if (tag == QStringLiteral("use")) {
            const QString href = n.attrs.value(QStringLiteral("href"));
            const Node* target = href.startsWith(QLatin1Char('#')) ? m_byId.value(href.mid(1)) : nullptr;
            if (target) {
                const Affine at = m * Affine::translate(number(n, "x"), number(n, "y"));
                if (target->tag == QStringLiteral("symbol") || target->tag == QStringLiteral("svg")) {
                    // Its viewBox fitted to the use's size (else its own).
                    const double w = size(n, "width") > 0 ? size(n, "width") : size(*target, "width");
                    const double h = size(n, "height") > 0 ? size(n, "height") : size(*target, "height");
                    const Affine inner = at * viewBoxMap(*target, w, h);
                    Paint symbolPaint = paint;
                    inherit(symbolPaint, ownStyle(*target));
                    for (const Node& k : target->kids) {
                        walk(k, inner, symbolPaint, opacity, layer, depth + 1);
                    }
                }
                else {
                    walk(*target, at, paint, opacity, layer, depth + 1);
                }
            }
            return;
        }
        if (tag == QStringLiteral("text")) {
            if (!m_clips.empty()) {
                note("texts and pictures in clip paths came in unclipped");
            }
            text(n, m, paint, opacity, layer);
            return;
        }
        if (tag == QStringLiteral("image")) {
            if (!m_clips.empty()) {
                note("texts and pictures in clip paths came in unclipped");
            }
            if (paint.visible) {
                image(n, m, layer, opacity);
            }
            return;
        }
        if (tag == QStringLiteral("foreignObject")) {
            skip("embedded objects");
            return;
        }
        std::vector<Contour> contours = shapeContours(n);
        if (contours.empty() || !paint.visible) {
            return;
        }
        // Markers (arrowheads, dots) go on its vertices, placed in its own space.
        auto marked = [](const QString& v) { return !v.trimmed().isEmpty() && v.trimmed() != QStringLiteral("none"); };
        const bool hasMarkers = marked(paint.markerStart) || marked(paint.markerMid) || marked(paint.markerEnd);
        const std::vector<Contour> userContours = hasMarkers ? contours : std::vector<Contour>{};
        Shape shape;
        shape.layer = layer;
        // Its box in its own space (what a pattern's objectBoundingBox units are of).
        std::array<double, 4> patternBox{1e300, 1e300, -1e300, -1e300};
        for (const Contour& c : contours) {
            for (const Segment& s : c.segments) {
                for (const Vec2& p : s.sample(8)) {
                    patternBox = {std::min(patternBox[0], p.x), std::min(patternBox[1], p.y), std::max(patternBox[2], p.x),
                                  std::max(patternBox[3], p.y)};
                }
            }
        }
        // Gradients are placed by the shape's own outline (its box), before it's transformed.
        const std::optional<GradientDef> fillGradient = gradientFor(paint.fill, m, contours, paint.fillOpacity * opacity);
        const std::optional<GradientDef> strokeGradient =
            gradientFor(paint.stroke, m, contours, paint.strokeOpacity * opacity);
        for (Contour& c : contours) {
            for (Segment& s : c.segments) {
                s = transformed(s, m);
            }
        }
        shape.contours = std::move(contours);
        Style& st = shape.style;
        st.fill = color(paint.fill, paint, paint.fillOpacity * opacity);
        st.stroke = color(paint.stroke, paint, paint.strokeOpacity * opacity);
        if (!st.stroke) {
            if (const Node* pattern = patternOf(paint.stroke)) {  // (a patterned line: the tile's colour)
                if (const auto tile = tileOf(*pattern, m, patternBox, layer, depth)) {
                    for (const Shape& s : tile->shapes) {
                        if (s.style.fill || s.style.stroke) {
                            st.stroke = s.style.fill ? *s.style.fill : *s.style.stroke;
                            note("patterned lines came in in their pattern's colour");
                            break;
                        }
                    }
                }
            }
        }
        st.fillGradient = st.fill ? fillGradient : std::nullopt;
        st.strokeGradient = st.stroke ? strokeGradient : std::nullopt;
        auto byUrl = [](const QString& p) { return p.trimmed().startsWith(QStringLiteral("url(")); };
        if ((st.fill && !st.fillGradient && byUrl(paint.fill)) || (st.stroke && !st.strokeGradient && byUrl(paint.stroke))) {
            note("some gradients came in as plain colours (their average)");
        }
        st.evenOdd = paint.fillRule == QStringLiteral("evenodd");
        st.strokeWidth = paint.strokeWidth * std::sqrt(std::abs(m.det()));
        const std::vector<double> dashes = numbers(paint.dash);
        if (dashes.size() >= 2) {
            const double w = std::max(paint.strokeWidth, 1e-9);
            st.dash = dashes.size() >= 6 ? core::Dash::DashDotDot
                    : dashes.size() >= 4 ? core::Dash::DashDot
                    : dashes[0] <= 1.5 * w ? core::Dash::Dotted
                                           : core::Dash::Dashed;
        }
        // Our own exports: their Construction layer.
        const QString top = QString::fromStdString(layer).section(QLatin1Char('/'), -1);
        st.construction = top.compare(QStringLiteral("Construction"), Qt::CaseInsensitive) == 0;
        // A pattern: hatched (a tile of lines), or its tiles repeated across it (after it, so
        // over its fill), or its average colour.
        std::vector<Shape> tiles;
        if (const Node* pattern = patternOf(paint.fill)) {
            st.fill = std::nullopt;
            fillWithPattern(*pattern, m, patternBox, shape, paint.fillOpacity * opacity, layer, depth, tiles);
        }
        if (!cutToClips(shape, m_clips)) {
            return;  // (all of it outside)
        }
        if (!layer.empty() && std::ranges::find(m_layerOrder, layer) == m_layerOrder.end()) {
            m_layerOrder.push_back(layer);
        }
        // (a pattern of tiles with no line round it: just the tiles)
        if (st.fill || st.stroke || st.construction || tiles.empty()) {
            m_out.shapes.push_back(std::move(shape));
        }
        for (Shape& tile : tiles) {
            m_out.shapes.push_back(std::move(tile));
        }
        if (hasMarkers) {
            markers(userContours, m, paint, opacity, layer, depth);
        }
    }

    // The <pattern> a paint ("url(#id)") refers to, if it's one.
    const Node* patternOf(const QString& paintText) const
    {
        static const QRegularExpression ref(QStringLiteral("^\\s*url\\(\\s*['\"]?#([^)'\"]+)"));
        const auto match = ref.match(paintText);
        const Node* p = match.hasMatch() ? m_byId.value(match.captured(1)) : nullptr;
        return p && p->tag == QStringLiteral("pattern") ? p : nullptr;
    }

    // A pattern's tile: its shapes in drawing coordinates (cut to the tile, as SVG cuts them),
    // where the tiles repeat (origin, and the steps across and up), and its tile's outline.
    struct Tile
    {
        std::vector<Shape> shapes;
        Vec2 origin, across, up;
    };
    std::optional<Tile> tileOf(const Node& pattern, const Affine& m, const std::array<double, 4>& box,
                               const std::string& layer, int depth)
    {
        // Its chain of templates (href): attributes and contents from the nearest that has them.
        std::vector<const Node*> chain{&pattern};
        for (int hops = 0; hops < 8; ++hops) {
            const QString href = chain.back()->attrs.value(QStringLiteral("href"));
            const Node* next = href.startsWith(QLatin1Char('#')) ? m_byId.value(href.mid(1)) : nullptr;
            if (!next || std::ranges::find(chain, next) != chain.end()) {
                break;
            }
            chain.push_back(next);
        }
        auto holder = [&](const char* name) -> const Node* {
            for (const Node* n : chain) {
                if (n->attrs.contains(QLatin1String(name))) {
                    return n;
                }
            }
            return nullptr;
        };
        auto attr = [&](const char* name, const QString& fallback) {
            const Node* n = holder(name);
            return n ? n->attrs.value(QLatin1String(name)) : fallback;
        };
        const Node* content = chain.front();
        for (const Node* n : chain) {
            if (std::ranges::any_of(n->kids, [](const Node& k) { return k.tag != QStringLiteral("#text"); })) {
                content = n;
                break;
            }
        }
        const double bw = box[2] - box[0], bh = box[3] - box[1];
        const bool boxUnits = attr("patternUnits", QStringLiteral("objectBoundingBox")) != QStringLiteral("userSpaceOnUse");
        auto value = [&](const char* name, bool horizontal) {
            const QString text = attr(name, QStringLiteral("0")).trimmed();
            const double v = firstOr(numbers(text), 0.0) / (text.endsWith(QLatin1Char('%')) ? 100.0 : 1.0);
            return boxUnits ? (horizontal ? box[0] + v * bw : box[1] + v * bh) : v;
        };
        auto extent = [&](const char* name, bool horizontal) {
            const QString text = attr(name, QStringLiteral("0")).trimmed();
            const double v = firstOr(numbers(text), 0.0) / (text.endsWith(QLatin1Char('%')) ? 100.0 : 1.0);
            return boxUnits ? v * (horizontal ? bw : bh) : v;
        };
        const double x = value("x", true), y = value("y", false), w = extent("width", true), h = extent("height", false);
        if (!(w > 1e-12 && h > 1e-12)) {
            return std::nullopt;
        }
        const Affine turn = transformOf(attr("patternTransform", QString()));
        Affine contentMap = Affine::translate(x, y);
        if (const Node* vb = holder("viewBox")) {
            contentMap = contentMap * viewBoxMap(*vb, w, h);
        }
        else if (attr("patternContentUnits", QStringLiteral("userSpaceOnUse")) == QStringLiteral("objectBoundingBox")) {
            contentMap = Affine::translate(x, y) * Affine::scale(bw, bh);
        }
        Tile tile;
        tile.origin = m(turn({x, y}));
        tile.across = m.linear(turn.linear({w, 0}));
        tile.up = m.linear(turn.linear({0, h}));

        // Its contents, walked as any other shapes, then taken back out of the drawing.
        const size_t shapes = m_out.shapes.size(), texts = m_out.texts.size(), images = m_out.images.size();
        std::vector<std::vector<Area>> clips = std::move(m_clips);
        m_clips.clear();
        Paint paint;
        inherit(paint, ownStyle(*content));
        for (const Node& k : content->kids) {
            walk(k, m * turn * contentMap, paint, 1.0, layer, depth + 1);
        }
        m_clips = std::move(clips);
        if (m_out.texts.size() > texts || m_out.images.size() > images) {
            note("texts and pictures in patterns were left out");
            m_out.texts.resize(texts);
            m_out.images.resize(images);
        }
        tile.shapes.assign(std::make_move_iterator(m_out.shapes.begin() + static_cast<long>(shapes)),
                           std::make_move_iterator(m_out.shapes.end()));
        m_out.shapes.resize(shapes);
        // Cut to the tile (what overflows it doesn't show; a hair bigger, to keep lines on its edges).
        Area cell;
        constexpr double kHair = 1e-6;
        const Vec2 o = sub(tile.origin, mul(add(tile.across, tile.up), kHair));
        const Vec2 a = add(o, mul(tile.across, 1 + 2 * kHair)), b = add(a, mul(tile.up, 1 + 2 * kHair));
        const Vec2 c = add(o, mul(tile.up, 1 + 2 * kHair));
        cell.contours.push_back({{line(o, a), line(a, b), line(b, c), line(c, o)}, true});
        cell.rings = ringsOf(cell.contours);
        std::erase_if(tile.shapes, [&](Shape& s) { return !cutToClips(s, {{cell}}); });
        return tile;
    }

    // A tile of lines only (straight, stroked, one colour and width, nothing filled) as a hatch:
    // each line the family it repeats into. None if it isn't such a tile.
    static std::optional<std::pair<core::Hatch, Color>> tileHatch(const Tile& tile)
    {
        std::optional<Color> colour;
        double width = 0;
        std::vector<std::pair<Vec2, Vec2>> lines;
        // (a fill counts only if it covers something: a lone line "filled" by default doesn't)
        auto covers = [](const Shape& s) {
            double area = 0;
            for (const auto& ring : ringsOf(s.contours)) {
                for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
                    area += cross(ring[j], ring[i]);
                }
            }
            return std::abs(area) > 1e-9;
        };
        for (const Shape& s : tile.shapes) {
            if ((s.style.fill && covers(s)) || !s.style.stroke || (colour && !(*colour == *s.style.stroke))
                || (!lines.empty() && std::abs(width - s.style.strokeWidth) > 1e-9)) {
                return std::nullopt;
            }
            colour = s.style.stroke;
            width = s.style.strokeWidth;
            for (const Contour& c : s.contours) {
                if (c.lineless) {
                    continue;
                }
                for (const Segment& seg : c.segments) {
                    if (seg.kind == Segment::Kind::Line || (seg.kind == Segment::Kind::Cubic && straightSegment(seg))) {
                        lines.push_back({seg.start(), seg.end()});
                    }
                    else {
                        return std::nullopt;
                    }
                }
            }
        }
        if (!colour || lines.empty()) {
            return std::nullopt;
        }
        const Vec2 A = tile.across, B = tile.up;
        const double area = std::abs(cross(A, B));
        if (area < 1e-12) {
            return std::nullopt;
        }
        core::Hatch h;
        h.pattern = "SVG pattern";
        h.width = width;
        for (const auto& [a, b] : lines) {
            const double l = len(sub(b, a));
            if (l < 1e-9) {
                continue;
            }
            const Vec2 u = mul(sub(b, a), 1.0 / l);
            // The step along the line from one tile's copy to the next in line (the shortest grid
            // step that way), and the distance between neighbouring lines (the tile's area over it).
            double period = 0;
            for (int i = -12; i <= 12; ++i) {
                for (int j = -12; j <= 12; ++j) {
                    const Vec2 v = add(mul(A, i), mul(B, j));
                    const double lv = len(v);
                    if (lv > 1e-9 && dot(v, u) > 0 && std::abs(cross(v, u)) < 1e-7 * lv && (period == 0 || lv < period)) {
                        period = lv;
                    }
                }
            }
            if (period == 0) {
                return std::nullopt;  // (at an angle that never repeats along itself)
            }
            const double spacing = area / period;
            std::optional<Vec2> next;
            for (int i = -12; i <= 12 && !next; ++i) {
                for (int j = -12; j <= 12 && !next; ++j) {
                    const Vec2 v = add(mul(A, i), mul(B, j));
                    if (std::abs(cross(u, v) - spacing) < 1e-7 * std::max(1.0, spacing)) {
                        next = v;
                    }
                }
            }
            if (!next) {
                return std::nullopt;
            }
            core::HatchLine family;
            family.angle = std::atan2(u.y, u.x) * 180 / kPi;
            family.x = a.x;
            family.y = a.y;
            family.dx = std::fmod(dot(*next, u), period);
            family.dy = spacing;
            if (l < period - 1e-9) {
                family.dashes = {l, -(period - l)};
            }
            // (a line the tile has twice, say along both its edges: the same family)
            const bool same = std::ranges::any_of(h.lines, [&](const core::HatchLine& o) {
                if (std::abs(std::remainder(o.angle - family.angle, 360.0)) > 1e-6 || o.dashes.size() != family.dashes.size()) {
                    return false;
                }
                const Vec2 d = sub(Vec2{family.x, family.y}, Vec2{o.x, o.y});
                for (int i = -12; i <= 12; ++i) {
                    for (int j = -12; j <= 12; ++j) {
                        if (len(sub(d, add(mul(A, i), mul(B, j)))) < 1e-7 * std::max(1.0, len(d))) {
                            return true;
                        }
                    }
                }
                return false;
            });
            if (!same) {
                h.lines.push_back(std::move(family));
            }
        }
        if (h.lines.empty()) {
            return std::nullopt;
        }
        return std::pair{h, *colour};
    }

    // Fills a shape with a pattern: hatched where its tile is lines, else its tiles repeated
    // across it (into `tiles`, cut to it), else (too many) the tile's average colour.
    void fillWithPattern(const Node& pattern, const Affine& m, const std::array<double, 4>& box, Shape& shape,
                         double opacity, const std::string& layer, int depth, std::vector<Shape>& tiles)
    {
        const std::optional<Tile> tile = tileOf(pattern, m, box, layer, depth);
        if (!tile || tile->shapes.empty()) {
            return;  // (an empty tile: nothing shows)
        }
        auto faded = [&](Color c) {
            c.a = static_cast<std::uint8_t>(std::lround(c.a * std::clamp(opacity, 0.0, 1.0)));
            return c;
        };
        if (auto hatched = tileHatch(*tile)) {
            shape.style.fill = faded(hatched->second);
            shape.style.hatch = hatched->first;
            return;
        }
        // The tiles over the shape's box: which copies (the box in the tiles' own grid).
        const Area area{shape.contours, ringsOf(shape.contours), shape.style.evenOdd};
        const double det = cross(tile->across, tile->up);
        double i0 = 1e300, i1 = -1e300, j0 = 1e300, j1 = -1e300;
        for (const auto& ring : area.rings) {
            for (const Vec2& p : ring) {
                const Vec2 d = sub(p, tile->origin);
                const double i = cross(d, tile->up) / det, j = cross(tile->across, d) / det;
                i0 = std::min(i0, i);
                i1 = std::max(i1, i);
                j0 = std::min(j0, j);
                j1 = std::max(j1, j);
            }
        }
        const long long iFrom = static_cast<long long>(std::floor(i0)), iTo = static_cast<long long>(std::ceil(i1));
        const long long jFrom = static_cast<long long>(std::floor(j0)), jTo = static_cast<long long>(std::ceil(j1));
        size_t pieces = 0;
        for (const Shape& s : tile->shapes) {
            for (const Contour& c : s.contours) {
                pieces += c.segments.size();
            }
        }
        const double copies = double(iTo - iFrom) * double(jTo - jFrom);
        if (copies * double(pieces) > 20000 || copies > 4000) {
            // Too many to draw one by one: the tile's colour.
            for (const Shape& s : tile->shapes) {
                if (s.style.fill || s.style.stroke) {
                    shape.style.fill = faded(s.style.fill ? *s.style.fill : *s.style.stroke);
                    break;
                }
            }
            note("patterns too fine to draw tile by tile came in as their colour");
            return;
        }
        for (long long i = iFrom; i < iTo; ++i) {
            for (long long j = jFrom; j < jTo; ++j) {
                const Vec2 shift = add(mul(tile->across, double(i)), mul(tile->up, double(j)));
                const Affine move = Affine::translate(shift.x, shift.y);
                for (const Shape& s : tile->shapes) {
                    Shape copy = s;
                    copy.layer = layer;
                    for (Contour& c : copy.contours) {
                        for (Segment& seg : c.segments) {
                            seg = transformed(seg, move);
                        }
                    }
                    auto moveArea = [&](Area& a) {
                        for (Contour& c : a.contours) {
                            for (Segment& seg : c.segments) {
                                seg = transformed(seg, move);
                            }
                        }
                        for (auto& ring : a.rings) {
                            for (Vec2& p : ring) {
                                p = add(p, shift);
                            }
                        }
                    };
                    if (copy.fillArea) {
                        moveArea(*copy.fillArea);
                    }
                    for (auto& clip : copy.clips) {
                        for (Area& a : clip) {
                            moveArea(a);
                        }
                    }
                    for (auto* g : {&copy.style.fillGradient, &copy.style.strokeGradient}) {
                        if (*g) {
                            (*g)->start = add((*g)->start, shift);
                            (*g)->end = add((*g)->end, shift);
                            (*g)->center = add((*g)->center, shift);
                            if ((*g)->focal) {
                                *(*g)->focal = add(*(*g)->focal, shift);
                            }
                        }
                    }
                    // Cut to the shape (and whatever clips it's in).
                    std::vector<std::vector<Area>> clips{{area}};
                    clips.insert(clips.end(), m_clips.begin(), m_clips.end());
                    if (cutToClips(copy, clips)) {
                        tiles.push_back(std::move(copy));
                    }
                }
            }
        }
    }

    // Cuts a shape to clips, one after another; false if nothing of it is left.
    static bool cutToClips(Shape& shape, const std::vector<std::vector<Area>>& clips)
    {
        if (clips.empty()) {
            return true;
        }
        Area original = shape.fillArea ? *shape.fillArea : Area{shape.contours, ringsOf(shape.contours), shape.style.evenOdd};
        for (const std::vector<Area>& clip : clips) {
            auto inside = [&](Vec2 p) {
                if (!inArea(original, p)) {
                    return false;
                }
                return std::ranges::all_of(shape.clips, [&](const std::vector<Area>& c) {
                    return std::ranges::any_of(c, [&](const Area& a) { return inArea(a, p); });
                });
            };
            shape.contours = cutTo(shape.contours, clip, shape.style.fill.has_value(), inside);
            shape.clips.push_back(clip);
        }
        shape.fillArea = std::move(original);
        return !shape.contours.empty();
    }

    // The markers on a shape's vertices: its first (marker-start), its last (marker-end), the rest
    // (marker-mid). Each marker's contents drawn there, turned with the path (orient="auto") or
    // as given, sized to the stroke (markerUnits), its viewBox fitted to its size, refX/refY on
    // the vertex.
    void markers(const std::vector<Contour>& contours, const Affine& m, const Paint& paint, double opacity,
                 const std::string& layer, int depth)
    {
        struct Vertex
        {
            Vec2 p;
            std::optional<Vec2> in, out;  // the path's direction coming in, going out
        };
        std::vector<Vertex> vertices;
        for (const Contour& c : contours) {
            if (c.segments.empty()) {
                continue;
            }
            vertices.push_back({c.segments.front().start(), std::nullopt, startDirection(c.segments.front())});
            for (size_t i = 0; i < c.segments.size(); ++i) {
                Vertex v{c.segments[i].end(), endDirection(c.segments[i]), std::nullopt};
                if (i + 1 < c.segments.size()) {
                    v.out = startDirection(c.segments[i + 1]);
                }
                else if (c.closed) {
                    v.out = startDirection(c.segments.front());  // (closed: round to the start again)
                }
                vertices.push_back(v);
            }
        }
        if (vertices.empty()) {
            return;
        }
        for (size_t i = 0; i < vertices.size(); ++i) {
            const bool isStart = i == 0, isEnd = i + 1 == vertices.size();
            const QString& which = isStart ? paint.markerStart : isEnd ? paint.markerEnd : paint.markerMid;
            static const QRegularExpression ref(QStringLiteral("url\\(\\s*['\"]?#([^)'\"]+)"));
            const auto match = ref.match(which);
            const Node* marker = match.hasMatch() ? m_byId.value(match.captured(1)) : nullptr;
            if (!marker || marker->tag != QStringLiteral("marker")) {
                continue;
            }
            const Vertex& v = vertices[i];
            // Its angle: along the path there (between its ways in and out), or as given.
            const QString orient = marker->attrs.value(QStringLiteral("orient"), QStringLiteral("0")).trimmed();
            double angle = 0.0;
            if (orient.startsWith(QStringLiteral("auto"))) {
                Vec2 d{0, 0};
                for (const auto& part : {v.in, v.out}) {
                    if (part && len(*part) > 0) {
                        d = add(d, mul(*part, 1.0 / len(*part)));
                    }
                }
                if (len(d) < 1e-12 && v.in) {
                    d = *v.in;  // (turning straight back)
                }
                angle = std::atan2(d.y, d.x);
                if (isStart && orient == QStringLiteral("auto-start-reverse")) {
                    angle += kPi;
                }
            }
            else {
                angle = firstOr(numbers(orient), 0.0) * kPi / 180;
            }
            const double scale = marker->attrs.value(QStringLiteral("markerUnits")) == QStringLiteral("userSpaceOnUse") ? 1.0 : paint.strokeWidth;
            const double w = size(*marker, "markerWidth") > 0 ? size(*marker, "markerWidth") : 3.0;
            const double h = size(*marker, "markerHeight") > 0 ? size(*marker, "markerHeight") : 3.0;
            const Affine fit = marker->attrs.contains(QStringLiteral("viewBox")) ? viewBoxMap(*marker, w, h) : Affine{};
            const Vec2 refPoint = fit({number(*marker, "refX"), number(*marker, "refY")});
            const Affine place = m * Affine::translate(v.p.x, v.p.y) * Affine::rotate(angle) * Affine::scale(scale, scale)
                               * Affine::translate(-refPoint.x, -refPoint.y) * fit;
            Paint markerPaint;  // (its contents' style comes from the marker, not the path)
            inherit(markerPaint, ownStyle(*marker));
            markerPaint.markerStart = markerPaint.markerMid = markerPaint.markerEnd = QString();
            for (const Node& k : marker->kids) {
                walk(k, place, markerPaint, opacity, layer, depth + 1);
            }
        }
    }

    std::vector<Contour> shapeContours(const Node& n) const
    {
        const QString& tag = n.tag;
        std::vector<Contour> out;
        if (tag == QStringLiteral("path")) {
            return pathContours(n.attrs.value(QStringLiteral("d")));
        }
        if (tag == QStringLiteral("rect")) {
            const double x = number(n, "x"), y = number(n, "y"), w = number(n, "width"), h = number(n, "height");
            if (!(w > 0 && h > 0)) {
                return out;
            }
            double rx = number(n, "rx", -1), ry = number(n, "ry", -1);
            if (rx < 0) rx = ry;
            if (ry < 0) ry = rx;
            rx = std::clamp(rx, 0.0, w / 2);
            ry = std::clamp(ry, 0.0, h / 2);
            Contour c;
            c.closed = true;
            if (rx <= 0 || ry <= 0) {
                const Vec2 p[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
                for (int i = 0; i < 4; ++i) {
                    c.segments.push_back(line(p[i], p[(i + 1) % 4]));
                }
            }
            else {
                // Sides and quarter ellipses (y down: angles turn clockwise on screen, as SVG's do).
                c.segments.push_back(line({x + rx, y}, {x + w - rx, y}));
                c.segments.push_back(ellipse({x + w - rx, y + ry}, rx, ry, 0, -kPi / 2, kPi / 2));
                c.segments.push_back(line({x + w, y + ry}, {x + w, y + h - ry}));
                c.segments.push_back(ellipse({x + w - rx, y + h - ry}, rx, ry, 0, 0, kPi / 2));
                c.segments.push_back(line({x + w - rx, y + h}, {x + rx, y + h}));
                c.segments.push_back(ellipse({x + rx, y + h - ry}, rx, ry, 0, kPi / 2, kPi / 2));
                c.segments.push_back(line({x, y + h - ry}, {x, y + ry}));
                c.segments.push_back(ellipse({x + rx, y + ry}, rx, ry, 0, kPi, kPi / 2));
            }
            out.push_back(std::move(c));
        }
        else if (tag == QStringLiteral("circle") || tag == QStringLiteral("ellipse")) {
            const double cx = number(n, "cx"), cy = number(n, "cy");
            const double rx = tag == QStringLiteral("circle") ? number(n, "r") : number(n, "rx");
            const double ry = tag == QStringLiteral("circle") ? rx : number(n, "ry");
            if (rx > 0 && ry > 0) {
                out.push_back({{ellipse({cx, cy}, rx, ry, 0, 0, kTwoPi)}, true});
            }
        }
        else if (tag == QStringLiteral("line")) {
            out.push_back({{line({number(n, "x1"), number(n, "y1")}, {number(n, "x2"), number(n, "y2")})}, false});
        }
        else if (tag == QStringLiteral("polyline") || tag == QStringLiteral("polygon")) {
            const std::vector<double> v = numbers(n.attrs.value(QStringLiteral("points")));
            Contour c;
            c.closed = tag == QStringLiteral("polygon");
            std::vector<Vec2> pts;
            for (size_t i = 0; i + 1 < v.size(); i += 2) {
                pts.push_back({v[i], v[i + 1]});
            }
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                c.segments.push_back(line(pts[i], pts[i + 1]));
            }
            if (c.closed && pts.size() > 2) {
                c.segments.push_back(line(pts.back(), pts.front()));
            }
            if (!c.segments.empty()) {
                out.push_back(std::move(c));
            }
        }
        return out;
    }

    static std::vector<Contour> pathContours(const QString& d)
    {
        std::vector<Contour> out;
        // Tokens: commands and numbers (arc flags may be packed: "a1 1 0 0110 10").
        size_t i = 0;
        const std::string s = d.toStdString();
        auto skipSpace = [&] {
            while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) {
                ++i;
            }
        };
        auto readNumber = [&](double& v) {
            skipSpace();
            const size_t start = i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            bool digits = false, dotSeen = false;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || (s[i] == '.' && !dotSeen))) {
                dotSeen = dotSeen || s[i] == '.';
                digits = digits || s[i] != '.';
                ++i;
            }
            if (digits && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                size_t j = i + 1;
                if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
                if (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
                    i = j;
                    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                }
            }
            if (!digits) {
                i = start;
                return false;
            }
            v = std::stod(s.substr(start, i - start));
            return true;
        };
        auto readFlag = [&](double& v) {
            skipSpace();
            if (i < s.size() && (s[i] == '0' || s[i] == '1')) {
                v = s[i] - '0';
                ++i;
                return true;
            }
            return false;
        };

        Contour cur;
        Vec2 pos, start, lastControl;
        char lastCmd = 0;
        auto finish = [&](bool closed) {
            if (!cur.segments.empty()) {
                cur.closed = closed;
                out.push_back(std::move(cur));
            }
            cur = Contour{};
        };
        char cmd = 0;
        while (true) {
            skipSpace();
            if (i >= s.size()) {
                break;
            }
            if (std::isalpha(static_cast<unsigned char>(s[i]))) {
                cmd = s[i++];
            }
            else if (cmd == 0) {
                break;  // (numbers before any command: malformed)
            }
            const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
            const char C = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
            auto at = [&](double x, double y) { return rel ? Vec2{pos.x + x, pos.y + y} : Vec2{x, y}; };
            double v[7];
            if (C == 'Z') {
                if (len(sub(pos, start)) > 1e-12) {
                    cur.segments.push_back(line(pos, start));
                }
                pos = start;
                finish(true);
                lastCmd = 'Z';
                continue;
            }
            auto read = [&](int count) {
                for (int k = 0; k < count; ++k) {
                    if (!readNumber(v[k])) {
                        return false;
                    }
                }
                return true;
            };
            if (C == 'M') {
                if (!read(2)) break;
                finish(false);
                pos = start = at(v[0], v[1]);
                cmd = rel ? 'l' : 'L';  // (further pairs are lines)
                lastCmd = 'M';
                continue;
            }
            if (C == 'L') {
                if (!read(2)) break;
                const Vec2 p = at(v[0], v[1]);
                cur.segments.push_back(line(pos, p));
                pos = p;
            }
            else if (C == 'H') {
                if (!read(1)) break;
                const Vec2 p{rel ? pos.x + v[0] : v[0], pos.y};
                cur.segments.push_back(line(pos, p));
                pos = p;
            }
            else if (C == 'V') {
                if (!read(1)) break;
                const Vec2 p{pos.x, rel ? pos.y + v[0] : v[0]};
                cur.segments.push_back(line(pos, p));
                pos = p;
            }
            else if (C == 'C') {
                if (!read(6)) break;
                const Vec2 a = at(v[0], v[1]), b = at(v[2], v[3]), p = at(v[4], v[5]);
                cur.segments.push_back(cubic(pos, a, b, p));
                lastControl = b;
                pos = p;
            }
            else if (C == 'S') {
                if (!read(4)) break;
                const Vec2 a = (lastCmd == 'C' || lastCmd == 'S') ? sub(mul(pos, 2), lastControl) : pos;
                const Vec2 b = at(v[0], v[1]), p = at(v[2], v[3]);
                cur.segments.push_back(cubic(pos, a, b, p));
                lastControl = b;
                pos = p;
            }
            else if (C == 'Q' || C == 'T') {
                Vec2 q, p;
                if (C == 'Q') {
                    if (!read(4)) break;
                    q = at(v[0], v[1]);
                    p = at(v[2], v[3]);
                }
                else {
                    if (!read(2)) break;
                    q = (lastCmd == 'Q' || lastCmd == 'T') ? sub(mul(pos, 2), lastControl) : pos;
                    p = at(v[0], v[1]);
                }
                cur.segments.push_back(cubic(pos, lerp(pos, q, 2.0 / 3), lerp(p, q, 2.0 / 3), p));
                lastControl = q;
                pos = p;
            }
            else if (C == 'A') {
                if (!readNumber(v[0]) || !readNumber(v[1]) || !readNumber(v[2]) || !readFlag(v[3]) || !readFlag(v[4])
                    || !readNumber(v[5]) || !readNumber(v[6])) {
                    break;
                }
                const Vec2 p = at(v[5], v[6]);
                arcTo(cur, pos, p, v[0], v[1], v[2], v[3] != 0, v[4] != 0);
                pos = p;
            }
            else {
                break;  // unknown command
            }
            lastCmd = C;
        }
        finish(false);
        return out;
    }

    // SVG's endpoint arc as an ellipse round its centre (the SVG spec's appendix).
    static void arcTo(Contour& c, Vec2 p1, Vec2 p2, double rx, double ry, double angleDeg, bool large, bool sweep)
    {
        if (len(sub(p1, p2)) < 1e-12) {
            return;
        }
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx < 1e-12 || ry < 1e-12) {
            c.segments.push_back(line(p1, p2));
            return;
        }
        const double phi = angleDeg * kPi / 180, cp = std::cos(phi), sp = std::sin(phi);
        const double dx2 = (p1.x - p2.x) / 2, dy2 = (p1.y - p2.y) / 2;
        const double x1 = cp * dx2 + sp * dy2, y1 = -sp * dx2 + cp * dy2;
        const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
        if (lambda > 1) {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }
        const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        const double coef = (large == sweep ? -1.0 : 1.0) * std::sqrt(std::max(0.0, num / den));
        const double cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
        const Vec2 centre{cp * cx1 - sp * cy1 + (p1.x + p2.x) / 2, sp * cx1 + cp * cy1 + (p1.y + p2.y) / 2};
        const double t1 = std::atan2((y1 - cy1) / ry, (x1 - cx1) / rx);
        double dt = std::atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx) - t1;
        if (sweep && dt < 0) dt += kTwoPi;
        if (!sweep && dt > 0) dt -= kTwoPi;
        c.segments.push_back(ellipse(centre, rx, ry, phi, t1, dt));
    }

    const Node& m_root;
    double m_pxPerInch;
    QString m_folder;  // the file's own (for pictures it links to)
    QHash<QString, const Node*> m_byId;
    std::vector<const Node*> m_styles;
    std::vector<std::pair<QString, QHash<QString, QString>>> m_css;
    std::vector<std::string> m_layerOrder;
    Vec2 m_viewport;  // user units
    std::vector<std::vector<Area>> m_clips;  // the clip paths of the elements being walked
    Drawing m_out;
};
}  // namespace

Drawing readSvg(const QByteArray& data, double pxPerInch, const QString& folder)
{
    const Node root = parseXml(data);
    SvgReader reader(root, pxPerInch, folder);
    return reader.read();
}

// =============================================================================================
// DXF
// =============================================================================================

namespace
{
struct Record
{
    std::string type;
    std::vector<std::pair<int, std::string>> codes;
    std::vector<Record> vertices;  // a POLYLINE's

    const std::string* get(int code) const
    {
        for (const auto& [c, v] : codes) {
            if (c == code) {
                return &v;
            }
        }
        return nullptr;
    }
    double num(int code, double fallback = 0.0) const
    {
        const std::string* v = get(code);
        if (!v) {
            return fallback;
        }
        try {
            return std::stod(*v);
        }
        catch (...) {
            return fallback;
        }
    }
    std::vector<double> all(int code) const
    {
        std::vector<double> out;
        for (const auto& [c, v] : codes) {
            if (c == code) {
                try {
                    out.push_back(std::stod(v));
                }
                catch (...) {
                    out.push_back(0.0);
                }
            }
        }
        return out;
    }
};

std::string upper(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// B-spline control points in homogeneous form (x w, y w, w).
struct H
{
    double x, y, w;
};

void insertKnot(std::vector<double>& U, std::vector<H>& P, int p, double u)
{
    const int k = static_cast<int>(std::ranges::upper_bound(U, u) - U.begin()) - 1;
    std::vector<H> Q(P.size() + 1);
    for (int i = 0; i < static_cast<int>(Q.size()); ++i) {
        const size_t si = static_cast<size_t>(i);
        if (i <= k - p) {
            Q[si] = P[si];
        }
        else if (i >= k + 1) {
            Q[si] = P[si - 1];
        }
        else {
            const double span = U[static_cast<size_t>(i + p)] - U[si];
            const double a = span > 0 ? (u - U[si]) / span : 0.0;
            Q[si] = {P[si - 1].x * (1 - a) + P[si].x * a, P[si - 1].y * (1 - a) + P[si].y * a,
                     P[si - 1].w * (1 - a) + P[si].w * a};
        }
    }
    U.insert(U.begin() + k + 1, u);
    P = std::move(Q);
}

// A NURBS curve's point at u (de Boor).
Vec2 nurbsAt(const std::vector<double>& U, const std::vector<H>& P, int p, double u)
{
    const int n = static_cast<int>(P.size()) - 1;
    int k = p;
    while (k < n && U[static_cast<size_t>(k + 1)] <= u) {
        ++k;
    }
    std::vector<H> d(static_cast<size_t>(p + 1));
    for (int i = 0; i <= p; ++i) {
        d[static_cast<size_t>(i)] = P[static_cast<size_t>(k - p + i)];
    }
    for (int r = 1; r <= p; ++r) {
        for (int j = p; j >= r; --j) {
            const double lo = U[static_cast<size_t>(j + k - p)], hi = U[static_cast<size_t>(j + 1 + k - r)];
            const double a = hi > lo ? (u - lo) / (hi - lo) : 0.0;
            const H& x = d[static_cast<size_t>(j - 1)];
            H& y = d[static_cast<size_t>(j)];
            y = {x.x * (1 - a) + y.x * a, x.y * (1 - a) + y.y * a, x.w * (1 - a) + y.w * a};
        }
    }
    const H& r = d[static_cast<size_t>(p)];
    return r.w != 0.0 ? Vec2{r.x / r.w, r.y / r.w} : Vec2{r.x, r.y};
}

class DxfReader
{
public:
    DxfReader(const QByteArray& data, std::optional<double> mmPerUnit, const QString& folder)
        : m_mmPerUnit(mmPerUnit)
        , m_folder(folder)
    {
        // Pairs of lines: a group code, then its value.
        const QList<QByteArray> lines = data.split('\n');
        for (qsizetype i = 0; i + 1 < lines.size(); i += 2) {
            bool ok = false;
            const int code = lines[i].trimmed().toInt(&ok);
            if (!ok) {
                throw core::SketchError("not a DXF file (or a binary one, which isn't read)");
            }
            m_pairs.push_back({code, lines[i + 1].trimmed().toStdString()});
        }
        if (m_pairs.empty()) {
            throw core::SketchError("not a DXF file");
        }
    }

    Drawing read()
    {
        size_t i = 0;
        while (i < m_pairs.size()) {
            if (m_pairs[i].first == 0 && m_pairs[i].second == "SECTION" && i + 1 < m_pairs.size()) {
                const std::string name = m_pairs[i + 1].second;
                i += 2;
                if (name == "HEADER") {
                    // $-variables, each a 9 code and its value(s): not records.
                    for (size_t k = i; k + 1 < m_pairs.size() && !(m_pairs[k].first == 0 && m_pairs[k].second == "ENDSEC"); ++k) {
                        if (m_pairs[k].first == 9 && m_pairs[k].second == "$INSUNITS") {
                            try {
                                m_insunits = std::stoi(m_pairs[k + 1].second);
                            }
                            catch (...) {
                            }
                        }
                    }
                }
                std::vector<Record> records = readRecords(i);
                if (name == "TABLES") {
                    tables(records);
                }
                else if (name == "BLOCKS") {
                    blocks(records);
                }
                else if (name == "ENTITIES") {
                    m_entities = std::move(records);
                }
                else if (name == "OBJECTS") {
                    for (const Record& r : records) {
                        if (r.type == "IMAGEDEF" && r.get(5) && r.get(1)) {
                            m_imageFiles[upper(*r.get(5))] = *r.get(1);  // (by handle: what IMAGEs point to)
                        }
                    }
                }
            }
            else {
                ++i;
            }
        }
        double scale = 1.0;
        if (m_mmPerUnit) {
            scale = *m_mmPerUnit;
            m_out.units = "as chosen";
        }
        else {
            static const std::map<int, double> units = {
                {1, 25.4}, {2, 304.8}, {3, 1609344.0}, {4, 1.0},    {5, 10.0},  {6, 1000.0}, {7, 1e6},
                {8, 2.54e-5}, {9, 0.0254}, {10, 914.4}, {11, 1e-7}, {12, 1e-6}, {13, 1e-3}, {14, 100.0}};
            if (const auto it = units.find(m_insunits); it != units.end()) {
                scale = it->second;
                m_out.units = "from the file";
            }
            else {
                m_out.units = "millimetres (assumed: the file doesn't say)";
            }
        }
        const Affine base = Affine::scale(scale, scale);
        for (const Record& r : m_entities) {
            entity(r, base, std::string(), std::nullopt, 0);
        }
        return std::move(m_out);
    }

private:
    struct LayerInfo
    {
        int aci = 7;
        std::optional<Color> trueColor;
        std::string linetype = "CONTINUOUS";
        int lineweight = -3;
    };
    struct Block
    {
        Vec2 base;
        std::vector<Record> entities;
    };

    // Records (each starts at a 0 code) up to ENDSEC; a POLYLINE takes its VERTEXes.
    std::vector<Record> readRecords(size_t& i)
    {
        std::vector<Record> out;
        bool inPolyline = false;
        while (i < m_pairs.size()) {
            if (m_pairs[i].first != 0) {
                ++i;
                continue;
            }
            if (m_pairs[i].second == "ENDSEC") {
                ++i;
                break;
            }
            Record r;
            r.type = m_pairs[i].second;
            ++i;
            while (i < m_pairs.size() && m_pairs[i].first != 0) {
                r.codes.push_back(m_pairs[i]);
                ++i;
            }
            if (r.type == "VERTEX" && inPolyline) {
                out.back().vertices.push_back(std::move(r));
                continue;
            }
            if (r.type == "SEQEND") {
                inPolyline = false;
                continue;
            }
            inPolyline = r.type == "POLYLINE";
            out.push_back(std::move(r));
        }
        return out;
    }

    void tables(const std::vector<Record>& records)
    {
        for (const Record& r : records) {
            if (r.type == "STYLE" && r.get(2)) {
                // Its font: the family named in its extended data, else its font file's name
                // ("arial.ttf"; AutoCAD's own .shx fonts aren't installed anywhere else).
                std::string family = r.get(1000) ? *r.get(1000) : std::string();
                if (family.empty() && r.get(3)) {
                    const QFileInfo file(QString::fromStdString(*r.get(3)));
                    if (file.suffix().compare(QStringLiteral("shx"), Qt::CaseInsensitive) != 0) {
                        family = file.completeBaseName().toStdString();
                    }
                }
                m_textStyles[upper(*r.get(2))] = family;
                continue;
            }
            if (r.type != "LAYER" || !r.get(2)) {
                continue;
            }
            LayerInfo info;
            info.aci = std::abs(static_cast<int>(r.num(62, 7)));
            if (r.get(420)) {
                info.trueColor = trueColor(static_cast<long>(r.num(420)));
            }
            if (const std::string* lt = r.get(6)) {
                info.linetype = *lt;
            }
            info.lineweight = static_cast<int>(r.num(370, -3));
            m_layers[*r.get(2)] = info;
        }
    }

    void blocks(std::vector<Record>& records)
    {
        Block* current = nullptr;
        for (Record& r : records) {
            if (r.type == "BLOCK") {
                const std::string* name = r.get(2);
                current = name ? &m_blocks[*name] : nullptr;
                if (current) {
                    current->base = {r.num(10), r.num(20)};
                }
            }
            else if (r.type == "ENDBLK") {
                current = nullptr;
            }
            else if (current) {
                current->entities.push_back(std::move(r));
            }
        }
    }

    static Color trueColor(long rgb)
    {
        return {static_cast<std::uint8_t>((rgb >> 16) & 0xff), static_cast<std::uint8_t>((rgb >> 8) & 0xff),
                static_cast<std::uint8_t>(rgb & 0xff), 255};
    }

    static Color aci(int index)
    {
        return index == 7 ? Color{0, 0, 0, 255} : core::exporter::aciColor(index);  // (7: black on paper)
    }

    static core::Dash dashFor(const std::string& linetype)
    {
        const std::string t = upper(linetype);
        auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
        if (has("DASHDOTDOT") || has("DIVIDE") || has("PHANTOM")) {
            return core::Dash::DashDotDot;
        }
        if (has("DASHDOT") || has("CENTER")) {
            return core::Dash::DashDot;
        }
        if (has("DOT")) {
            return core::Dash::Dotted;
        }
        if (has("DASH") || has("HIDDEN")) {
            return core::Dash::Dashed;
        }
        return core::Dash::Solid;
    }

    void note(const std::string& text)
    {
        if (std::ranges::find(m_out.notes, text) == m_out.notes.end()) {
            m_out.notes.push_back(text);
        }
    }

    // A bulged polyline's vertices as segments.
    static Contour polyline(const std::vector<std::pair<Vec2, double>>& vs, bool closed)
    {
        Contour c;
        c.closed = closed;
        const size_t n = vs.size();
        const size_t count = closed ? n : (n > 0 ? n - 1 : 0);
        for (size_t k = 0; k < count && n > 1; ++k) {
            const Vec2 a = vs[k].first, b = vs[(k + 1) % n].first;
            const double bulge = vs[k].second;
            if (len(sub(a, b)) < 1e-12) {
                continue;
            }
            if (std::abs(bulge) < 1e-12) {
                c.segments.push_back(line(a, b));
                continue;
            }
            // An arc of 4 atan(bulge) (counter-clockwise if positive) between them.
            const double theta = 4 * std::atan(bulge);
            const Vec2 chord = sub(b, a);
            const double d = len(chord);
            const Vec2 normal{-chord.y / d, chord.x / d};
            const Vec2 centre = add(lerp(a, b, 0.5), mul(normal, d / 2 / std::tan(theta / 2)));
            const double radius = len(sub(a, centre));
            c.segments.push_back(ellipse(centre, radius, radius, 0, std::atan2(a.y - centre.y, a.x - centre.x), theta));
        }
        return c;
    }

    Contour spline(const Record& r)
    {
        const int degree = static_cast<int>(r.num(71, 3));
        const int flags = static_cast<int>(r.num(70));
        std::vector<double> U = r.all(40);
        const std::vector<double> xs = r.all(10), ys = r.all(20), ws = r.all(41);
        const std::vector<double> fx = r.all(11), fy = r.all(21);
        std::vector<H> P;
        for (size_t k = 0; k < xs.size() && k < ys.size(); ++k) {
            const double w = k < ws.size() && ws[k] > 0 ? ws[k] : 1.0;
            P.push_back({xs[k] * w, ys[k] * w, w});
        }
        Contour c;
        c.closed = (flags & 1) != 0;
        const size_t p = static_cast<size_t>(std::max(degree, 0));
        const bool usable = degree >= 1 && P.size() >= p + 1 && U.size() == P.size() + p + 1;
        if (usable) {
            const bool rational = std::ranges::any_of(P, [&](const H& h) { return std::abs(h.w - P.front().w) > 1e-12; });
            const double lo = U[p], hi = U[P.size()];
            const bool clamped = std::all_of(U.begin(), U.begin() + static_cast<long>(p) + 1, [&](double u) { return u == U.front(); })
                                 && std::all_of(U.end() - static_cast<long>(p) - 1, U.end(), [&](double u) { return u == U.back(); });
            if (!rational && degree <= 3 && clamped) {
                // Exactly: Bezier pieces (each inner knot raised to the degree's multiplicity).
                std::vector<double> inner;
                for (size_t k = p + 1; k < P.size(); ++k) {
                    if (U[k] > lo && U[k] < hi && (inner.empty() || inner.back() != U[k])) {
                        inner.push_back(U[k]);
                    }
                }
                std::vector<H> Q = P;
                for (double u : inner) {
                    const int have = static_cast<int>(std::ranges::count(U, u));
                    for (int k = have; k < degree; ++k) {
                        insertKnot(U, Q, degree, u);
                    }
                }
                auto pt = [&](size_t k) { return Vec2{Q[k].x / Q[k].w, Q[k].y / Q[k].w}; };
                for (size_t s0 = 0; s0 + p < Q.size(); s0 += p) {
                    if (degree == 1) {
                        c.segments.push_back(line(pt(s0), pt(s0 + 1)));
                    }
                    else if (degree == 2) {
                        const Vec2 a = pt(s0), q = pt(s0 + 1), b = pt(s0 + 2);
                        c.segments.push_back(cubic(a, lerp(a, q, 2.0 / 3), lerp(b, q, 2.0 / 3), b));
                    }
                    else {
                        c.segments.push_back(cubic(pt(s0), pt(s0 + 1), pt(s0 + 2), pt(s0 + 3)));
                    }
                }
                return c;
            }
            // Weighted, of a higher degree, or unclamped: closely, through points along it.
            std::vector<Vec2> pts;
            const int samples = 8 * static_cast<int>(P.size());
            for (int k = 0; k <= samples; ++k) {
                pts.push_back(nurbsAt(U, P, degree, lo + (hi - lo) * k / samples));
            }
            c.segments = throughPoints(pts, false);
            note("some splines were approximated (weighted or of a high degree)");
            return c;
        }
        if (fx.size() >= 2) {
            std::vector<Vec2> pts;
            for (size_t k = 0; k < fx.size() && k < fy.size(); ++k) {
                pts.push_back({fx[k], fy[k]});
            }
            c.segments = throughPoints(pts, c.closed);
            note("splines given by fit points only were drawn through them (closely, not exactly)");
        }
        return c;
    }

    // A picture (always linked to: its IMAGEDEF has the path), its bottom-left corner at 10/20, a
    // pixel's width and height along 11/21 and 12/22, 13/23 pixels across and up.
    void image(const Record& r, const Affine& m, const std::string& layer)
    {
        const std::string* handle = r.get(340);
        const auto file = handle ? m_imageFiles.find(upper(*handle)) : m_imageFiles.end();
        const QString path = file != m_imageFiles.end() ? findLinked(QString::fromStdString(file->second), m_folder) : QString();
        QFile picture(path);
        if (path.isEmpty() || !picture.open(QIODevice::ReadOnly)) {
            ++m_out.skipped["linked images not found"];
            return;
        }
        ImageDef def;
        try {
            def.image = images::fromBytes(picture.readAll(), QDir::toNativeSeparators(path), QFileInfo(path).fileName());
        }
        catch (const core::SketchError&) {
            ++m_out.skipped["images that can't be read"];
            return;
        }
        const double across = r.num(13, def.image.pixelWidth), up = r.num(23, def.image.pixelHeight);
        const Vec2 u = mul(Vec2{r.num(11), r.num(21)}, across), v = mul(Vec2{r.num(12), r.num(22)}, up);
        def.layer = layer;
        def.image.opacity = 0.5;  // (a reference: faded, as when inserted)
        if (!placeImage(def, m({r.num(10), r.num(20)}), m.linear(u), m.linear(v))) {
            note("stretched or skewed images came in keeping their own proportions");
        }
        m_out.images.push_back(std::move(def));
    }

    // TEXT (one line: 10/20 its start, or 11/21 the point it's justified on (72, 73)) and MTEXT
    // (lines: 10/20 its attachment point (71), its words in 3s then 1, with formatting codes).
    void text(const Record& r, const Affine& m, const std::string& layer, Color col)
    {
        const bool multi = r.type == "MTEXT";
        TextDef def;
        core::Text& t = def.text;
        QString words, font;
        if (multi) {
            QString raw;
            for (const auto& [code, value] : r.codes) {
                if (code == 3) {
                    raw += dxfString(value);
                }
            }
            if (const std::string* last = r.get(1)) {
                raw += dxfString(*last);
            }
            words = mtextPlain(raw, font);
        }
        else {
            words = r.get(1) ? dxfString(*r.get(1)) : QString();
        }
        words = words.trimmed();
        if (words.isEmpty()) {
            return;
        }
        // The point it's placed by, and how it's aligned on it.
        Vec2 at{r.num(10), r.num(20)};
        double angle = r.num(50) * kPi / 180;
        if (multi) {
            if (r.get(11)) {
                angle = std::atan2(r.num(21), r.num(11));  // (a direction rather than an angle)
            }
            const int attach = std::clamp(static_cast<int>(r.num(71, 1)), 1, 9);
            const int column = (attach - 1) % 3, row = (attach - 1) / 3;
            t.align = column == 0 ? core::TextAlign::Left : column == 1 ? core::TextAlign::Center : core::TextAlign::Right;
            t.valign = row == 0 ? core::TextVAlign::Top : row == 1 ? core::TextVAlign::Middle : core::TextVAlign::Bottom;
            if (r.num(41) > 0) {
                note("texts wrapped in a width came in as their own lines");
            }
        }
        else {
            const int h = static_cast<int>(r.num(72)), v = static_cast<int>(r.num(73));
            if ((h != 0 || v != 0) && r.get(11)) {
                at = {r.num(11), r.num(21)};
            }
            t.align = h == 1 || h == 4 ? core::TextAlign::Center : h == 2 ? core::TextAlign::Right : core::TextAlign::Left;
            t.valign = h == 4 || v == 2 ? core::TextVAlign::Middle : v == 3 ? core::TextVAlign::Top : core::TextVAlign::Baseline;
            if (h == 3 || h == 5) {
                at = {r.num(10), r.num(20)};
                note("texts fitted between two points came in at their size");
            }
            if (std::abs(r.num(41, 1.0) - 1.0) > 1e-6) {
                note("narrowed or widened texts came in at their font's width");
            }
        }
        t.text = words.toStdString();
        // Its font: from MTEXT's own codes, else its style's.
        if (font.isEmpty()) {
            const auto style = m_textStyles.find(upper(r.get(7) ? *r.get(7) : std::string("STANDARD")));
            font = style != m_textStyles.end() ? QString::fromStdString(style->second) : QString();
        }
        bool substituted = false;
        t.font = installedFamily({font}, substituted).toStdString();
        if (substituted && !font.isEmpty()) {
            note("fonts that aren't installed were replaced");
        }
        // Turned, scaled and mirrored as its block insert is; mirrored on its own (71: 2 backwards, 4 upside down).
        Vec2 dir{std::cos(angle), std::sin(angle)}, upDir{-dir.y, dir.x};
        const int mirror = multi ? 0 : static_cast<int>(r.num(71));
        if (mirror & 2) {
            dir = mul(dir, -1);
        }
        if (mirror & 4) {
            upDir = mul(upDir, -1);
        }
        const Vec2 right = m.linear(dir), up = m.linear(upDir);
        t.height = r.num(40, 2.5) * len(up);  // (a DXF text's height is its capitals')
        if (!(t.height > 1e-9)) {
            return;
        }
        orientText(t, right, up);
        t.hasColor = true;
        t.color = col;
        def.at = m(at);
        def.layer = layer;
        m_out.texts.push_back(std::move(def));
    }

    void entity(const Record& r, const Affine& m, const std::string& parentLayer, std::optional<Color> parentColor,
                int depth)
    {
        if (depth > 16) {
            return;
        }
        std::string layer = r.get(8) ? *r.get(8) : std::string("0");
        if (layer == "0" && !parentLayer.empty()) {
            layer = parentLayer;  // (a block's layer-0 entities take the insert's layer)
        }
        const LayerInfo info = m_layers[layer];
        // Colour: its own, else its layer's (by layer), or the insert's (by block).
        Color col = info.trueColor.value_or(aci(info.aci));
        if (r.get(420)) {
            col = trueColor(static_cast<long>(r.num(420)));
        }
        else if (r.get(62)) {
            const int index = static_cast<int>(r.num(62));
            if (index == 0) {
                col = parentColor.value_or(Color{0, 0, 0, 255});
            }
            else if (index != 256) {
                col = aci(std::abs(index));
            }
        }
        // Mirrored entities (an extrusion pointing down): x runs the other way in their plane.
        const bool flipped = r.num(230, 1.0) < 0;
        const Affine local = flipped ? m * Affine::scale(-1, 1) : m;

        const std::string& type = r.type;
        std::vector<Contour> contours;
        if (type == "INSERT") {
            const auto it = m_blocks.find(r.get(2) ? *r.get(2) : std::string());
            if (it == m_blocks.end()) {
                return;
            }
            const double sx = r.num(41, 1.0), sy = r.num(42, 1.0), rot = r.num(50) * kPi / 180;
            const int cols = std::max(1, static_cast<int>(r.num(70, 1))), rows = std::max(1, static_cast<int>(r.num(71, 1)));
            for (int row = 0; row < rows; ++row) {
                for (int column = 0; column < cols; ++column) {
                    const Vec2 offset = Affine::rotate(rot).linear({column * r.num(44), row * r.num(45)});
                    const Affine t = local * Affine::translate(r.num(10) + offset.x, r.num(20) + offset.y)
                                     * Affine::rotate(rot) * Affine::scale(sx, sy)
                                     * Affine::translate(-it->second.base.x, -it->second.base.y);
                    for (const Record& e : it->second.entities) {
                        entity(e, t, layer, col, depth + 1);
                    }
                }
            }
            return;
        }
        if (type == "LINE") {
            contours.push_back({{line({r.num(10), r.num(20)}, {r.num(11), r.num(21)})}, false});
        }
        else if (type == "CIRCLE") {
            contours.push_back({{ellipse({r.num(10), r.num(20)}, r.num(40), r.num(40), 0, 0, kTwoPi)}, true});
        }
        else if (type == "ARC") {
            const double a0 = r.num(50) * kPi / 180, a1 = r.num(51) * kPi / 180;
            double sweep = std::fmod(a1 - a0, kTwoPi);
            if (sweep <= 1e-12) {
                sweep += kTwoPi;
            }
            contours.push_back({{ellipse({r.num(10), r.num(20)}, r.num(40), r.num(40), 0, a0, sweep)}, false});
        }
        else if (type == "ELLIPSE") {
            const Vec2 major{r.num(11), r.num(21)};
            const double rx = len(major), ratio = r.num(40, 1.0);
            const double t0 = r.num(41, 0.0), t1 = r.num(42, kTwoPi);
            double sweep = std::fmod(t1 - t0, kTwoPi);
            if (sweep <= 1e-12) {
                sweep += kTwoPi;
            }
            // (its points are in world coordinates: a downward extrusion only runs it the other way)
            const Segment s = ellipse({r.num(10), r.num(20)}, rx, rx * ratio, std::atan2(major.y, major.x),
                                      flipped ? -t0 : t0, flipped ? -sweep : sweep);
            Contour c{{transformed(s, m)}, std::abs(sweep - kTwoPi) < 1e-9};
            std::vector<Contour> done{c};
            addShape(done, layer, info, col, r);
            return;
        }
        else if (type == "LWPOLYLINE") {
            std::vector<std::pair<Vec2, double>> vs;
            for (const auto& [code, value] : r.codes) {
                double v = 0;
                try {
                    v = std::stod(value);
                }
                catch (...) {
                }
                if (code == 10) {
                    vs.push_back({{v, 0}, 0});
                }
                else if (code == 20 && !vs.empty()) {
                    vs.back().first.y = v;
                }
                else if (code == 42 && !vs.empty()) {
                    vs.back().second = v;
                }
            }
            contours.push_back(polyline(vs, (static_cast<int>(r.num(70)) & 1) != 0));
        }
        else if (type == "POLYLINE") {
            const int flags = static_cast<int>(r.num(70));
            if (flags & (16 | 64)) {
                ++m_out.skipped["3D meshes"];
                return;
            }
            std::vector<std::pair<Vec2, double>> vs;
            for (const Record& v : r.vertices) {
                if (static_cast<int>(v.num(70)) & 16) {
                    continue;  // (a spline frame's control point)
                }
                vs.push_back({{v.num(10), v.num(20)}, v.num(42)});
            }
            contours.push_back(polyline(vs, (flags & 1) != 0));
        }
        else if (type == "SPLINE") {
            contours.push_back(spline(r));
        }
        else if (type == "IMAGE") {
            image(r, local, layer);
            return;
        }
        else if (type == "TEXT" || type == "MTEXT") {
            text(r, local, layer, col);
            return;
        }
        else if (type == "HATCH") {
            hatch(r, local, layer, col);
            return;
        }
        else if (type == "SOLID") {
            // A filled triangle or quadrilateral (its corners in the order 1, 2, 4, 3).
            const Vec2 p[4] = {{r.num(10), r.num(20)}, {r.num(11), r.num(21)}, {r.num(13), r.num(23)}, {r.num(12), r.num(22)}};
            Contour c;
            c.closed = true;
            for (int k = 0; k < 4; ++k) {
                const Vec2 a = local(p[k]), b = local(p[(k + 1) % 4]);
                if (len(sub(a, b)) > 1e-12) {
                    c.segments.push_back(line(a, b));
                }
            }
            if (c.segments.size() >= 3) {
                filledShape({std::move(c)}, layer, col, std::nullopt, std::nullopt);
            }
            return;
        }
        else {
            static const std::map<std::string, std::string> names = {
                {"TEXT", "texts"},          {"MTEXT", "texts"},       {"ATTRIB", "texts"},       {"ATTDEF", "texts"},
                {"DIMENSION", "dimensions"}, {"3DFACE", "3D faces"},
                {"IMAGE", "images"},        {"LEADER", "leaders"},    {"MLEADER", "leaders"},    {"POINT", "points"},
                {"WIPEOUT", "wipeouts"},    {"MULTILEADER", "leaders"}, {"TOLERANCE", "tolerances"}};
            if (type == "VIEWPORT" || type == "SEQEND" || type == "ENDBLK" || type == "BLOCK") {
                return;
            }
            const auto it = names.find(type);
            ++m_out.skipped[it != names.end() ? it->second : (type + " entities")];
            return;
        }
        for (Contour& c : contours) {
            for (Segment& s : c.segments) {
                s = transformed(s, local);
            }
        }
        std::erase_if(contours, [](const Contour& c) { return c.segments.empty(); });
        addShape(contours, layer, info, col, r);
    }

    // A filled area without an outline of its own (a hatch's, a solid's): solid, a gradient, or hatched.
    void filledShape(std::vector<Contour> contours, const std::string& layer, Color col, std::optional<core::Hatch> hatched,
                     std::optional<GradientDef> gradient)
    {
        Shape s;
        s.contours = std::move(contours);
        s.layer = layer;
        s.style.fill = col;
        s.style.evenOdd = true;  // (islands inside are left out, as "normal" hatching does)
        s.style.hatch = std::move(hatched);
        s.style.fillGradient = std::move(gradient);
        if (std::ranges::find(m_out.layers, layer) == m_out.layers.end()) {
            m_out.layers.push_back(layer);
        }
        m_out.shapes.push_back(std::move(s));
    }

    // HATCH: its boundary paths (polylines, or edges: lines, arcs, elliptical arcs, splines), then
    // its pattern's lines (stored turned and scaled, offsets in drawing coordinates), or a solid
    // or gradient fill. Its group codes come in order, so they're read in order.
    void hatch(const Record& r, const Affine& m, const std::string& layer, Color col)
    {
        const auto& c = r.codes;
        size_t i = 0;
        auto value = [&](size_t k) {
            try {
                return std::stod(c[k].second);
            }
            catch (...) {
                return 0.0;
            }
        };
        // The next pair with this code from here (and past it); -1 if there's none.
        auto next = [&](int code) -> double {
            while (i < c.size() && c[i].first != code) {
                ++i;
            }
            return i < c.size() ? value(i++) : -1.0;
        };
        auto point = [&](int xCode, int yCode) {
            const double x = next(xCode);
            return Vec2{x, next(yCode)};
        };
        std::vector<Contour> contours;
        const int paths = static_cast<int>(next(91));
        for (int p = 0; p < paths && i < c.size(); ++p) {
            const int flags = static_cast<int>(next(92));
            Contour ct;
            ct.closed = true;
            if (flags & 2) {  // a polyline
                const bool bulges = next(72) != 0;
                next(73);
                const int count = static_cast<int>(next(93));
                std::vector<std::pair<Vec2, double>> vs;
                for (int k = 0; k < count && i < c.size(); ++k) {
                    const Vec2 v = point(10, 20);
                    double bulge = 0.0;
                    if (bulges && i < c.size() && c[i].first == 42) {
                        bulge = value(i++);
                    }
                    vs.push_back({v, bulge});
                }
                ct = polyline(vs, true);
            }
            else {
                const int edges = static_cast<int>(next(93));
                for (int k = 0; k < edges && i < c.size(); ++k) {
                    const int kind = static_cast<int>(next(72));
                    if (kind == 1) {
                        const Vec2 a = point(10, 20), b = point(11, 21);
                        ct.segments.push_back(line(a, b));
                    }
                    else if (kind == 2 || kind == 3) {
                        const Vec2 centre = point(10, 20);
                        Vec2 major{1, 0};
                        double rx = 0, ry = 0;
                        if (kind == 2) {
                            rx = ry = next(40);
                        }
                        else {
                            major = point(11, 21);
                            rx = len(major);
                            ry = rx * next(40);
                        }
                        double a0 = next(50), a1 = next(51);
                        const bool ccw = i < c.size() && c[i].first == 73 ? value(i++) != 0 : true;
                        if (!ccw) {
                            std::tie(a0, a1) = std::pair{-a1, -a0};  // (stored mirrored: the same arc, run the other way)
                        }
                        double sweep = std::fmod(a1 - a0, 360.0);
                        if (sweep <= 1e-9) {
                            sweep += 360.0;
                        }
                        ct.segments.push_back(ellipse(centre, rx, ry, std::atan2(major.y, major.x), a0 * kPi / 180, sweep * kPi / 180));
                    }
                    else if (kind == 4) {
                        // As a SPLINE entity's codes, for the reader of those.
                        Record s;
                        s.type = "SPLINE";
                        const int degree = static_cast<int>(next(94));
                        const bool rational = next(73) != 0;
                        next(74);
                        const int knots = static_cast<int>(next(95)), controls = static_cast<int>(next(96));
                        s.codes.push_back({71, std::to_string(degree)});
                        for (int n = 0; n < knots; ++n) {
                            s.codes.push_back({40, std::to_string(next(40))});
                        }
                        for (int n = 0; n < controls; ++n) {
                            const Vec2 q = point(10, 20);
                            s.codes.push_back({10, std::to_string(q.x)});
                            s.codes.push_back({20, std::to_string(q.y)});
                            if (rational && i < c.size() && c[i].first == 42) {
                                s.codes.push_back({41, std::to_string(value(i++))});
                            }
                        }
                        if (i < c.size() && c[i].first == 97) {
                            const int fits = static_cast<int>(value(i++));
                            for (int n = 0; n < fits; ++n) {
                                const Vec2 q = point(11, 21);
                                s.codes.push_back({11, std::to_string(q.x)});
                                s.codes.push_back({21, std::to_string(q.y)});
                            }
                        }
                        const Contour piece = spline(s);
                        ct.segments.insert(ct.segments.end(), piece.segments.begin(), piece.segments.end());
                    }
                }
            }
            // (the objects it was made from: not needed)
            if (i < c.size() && c[i].first == 97) {
                const int sources = static_cast<int>(value(i++));
                for (int k = 0; k < sources && i < c.size() && c[i].first == 330; ++k) {
                    ++i;
                }
            }
            for (Segment& s : ct.segments) {
                s = transformed(s, m);
            }
            if (!ct.segments.empty()) {
                contours.push_back(std::move(ct));
            }
        }
        if (contours.empty()) {
            return;
        }
        const std::string name = r.get(2) ? *r.get(2) : std::string("HATCH");
        const bool solid = r.num(70) != 0 || upper(name) == "SOLID";
        // A gradient: two colours (or one and a tint of it), placed across the area.
        if (r.num(450) != 0) {
            filledShape(std::move(contours), layer, col, std::nullopt, std::nullopt);
            gradientHatch(r, m_out.shapes.back(), col);
            return;
        }
        if (solid) {
            filledShape(std::move(contours), layer, col, std::nullopt, std::nullopt);
            return;
        }
        // The pattern's lines: each its angle, base point and offset (in drawing coordinates),
        // and dashes, as drawn here: through the transform.
        i = 0;
        for (size_t k = 0; k < c.size(); ++k) {
            if (c[k].first == 78) {
                i = k;  // (the last: after the boundaries, before any seed points)
            }
        }
        const int count = i < c.size() && c[i].first == 78 ? static_cast<int>(value(i++)) : 0;
        core::Hatch h;
        h.pattern = name;
        for (int k = 0; k < count && i < c.size(); ++k) {
            const double angle = next(53) * kPi / 180;
            const Vec2 base = point(43, 44), offset = point(45, 46);
            const int dashCount = static_cast<int>(next(79));
            std::vector<double> dashes;
            for (int d = 0; d < dashCount; ++d) {
                dashes.push_back(next(49));
            }
            const Vec2 u0{std::cos(angle), std::sin(angle)};
            Vec2 u = m.linear(u0);
            const double s = len(u);
            if (s < 1e-12) {
                continue;
            }
            u = mul(u, 1.0 / s);
            const Vec2 n{-u.y, u.x};
            const Vec2 o = m.linear(offset), b = m(base);
            core::HatchLine l;
            l.angle = std::atan2(u.y, u.x) * 180 / kPi;
            l.x = b.x;
            l.y = b.y;
            l.dx = dot(o, u);
            l.dy = dot(o, n);
            for (double& d : dashes) {
                d *= s;
            }
            l.dashes = std::move(dashes);
            if (std::abs(l.dy) > 1e-9) {
                h.lines.push_back(std::move(l));
            }
        }
        if (h.lines.empty()) {
            filledShape(std::move(contours), layer, col, std::nullopt, std::nullopt);
            note("hatches without a pattern came in filled solid");
            return;
        }
        // A pattern of ours at its own angle and scale: kept so (to change them later).
        const double turn = std::atan2(m.linear({1, 0}).y, m.linear({1, 0}).x) * 180 / kPi;
        const double size = std::sqrt(std::abs(m.det()));
        for (const double unit : {1.0, 25.4}) {  // (metric patterns, or AutoCAD's inch ones)
            if (const auto known = core::hatch::builtin(name, r.num(52) + turn, r.num(41, 1.0) * size * unit)) {
                if (sameLines(core::hatch::placed(*known), h.lines)) {
                    filledShape(std::move(contours), layer, col, *known, std::nullopt);
                    return;
                }
            }
        }
        filledShape(std::move(contours), layer, col, std::move(h), std::nullopt);
    }

    // Two sets of hatch lines alike (to within a hair; angles a turn apart are alike).
    static bool sameLines(const std::vector<core::HatchLine>& a, const std::vector<core::HatchLine>& b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        auto near = [](double x, double y) { return std::abs(x - y) <= 1e-6 * std::max({1.0, std::abs(x), std::abs(y)}); };
        for (size_t k = 0; k < a.size(); ++k) {
            const double turn = std::remainder(a[k].angle - b[k].angle, 360.0);
            if (std::abs(turn) > 1e-6 || !near(a[k].x, b[k].x) || !near(a[k].y, b[k].y) || !near(a[k].dx, b[k].dx)
                || !near(a[k].dy, b[k].dy) || a[k].dashes.size() != b[k].dashes.size()) {
                return false;
            }
            for (size_t d = 0; d < a[k].dashes.size(); ++d) {
                if (!near(a[k].dashes[d], b[k].dashes[d])) {
                    return false;
                }
            }
        }
        return true;
    }

    // A gradient hatch (450): its colours (421s, else ACI 63s; one colour: tinted, 462) across the
    // area at its angle (460, radians): linear ones from side to side, spherical ones from the middle.
    void gradientHatch(const Record& r, Shape& shape, Color col)
    {
        std::vector<Color> colours;
        bool seen = false;
        for (const auto& [code, v] : r.codes) {
            seen = seen || code == 450;
            if (seen && code == 421) {
                colours.push_back(trueColor(std::stol(v)));
            }
        }
        if (colours.empty()) {
            colours.push_back(col);
        }
        if (colours.size() == 1) {
            // Its tint: the colour towards white (above a half) or towards black (below).
            const double tint = std::clamp(r.num(462, 1.0), 0.0, 1.0);
            const double toward = tint >= 0.5 ? 255.0 : 0.0, amount = std::abs(tint - 0.5) * 2;
            auto mix = [&](std::uint8_t v) { return static_cast<std::uint8_t>(std::lround(v + (toward - v) * amount)); };
            const Color a = colours.front();
            colours.push_back(Color{mix(a.r), mix(a.g), mix(a.b), a.a});
        }
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (const Contour& ct : shape.contours) {
            for (const Segment& s : ct.segments) {
                for (const Vec2& p : s.sample(8)) {
                    x0 = std::min(x0, p.x);
                    y0 = std::min(y0, p.y);
                    x1 = std::max(x1, p.x);
                    y1 = std::max(y1, p.y);
                }
            }
        }
        const Vec2 centre{(x0 + x1) / 2, (y0 + y1) / 2};
        const std::string name = upper(r.get(470) ? *r.get(470) : std::string("LINEAR"));
        const Color& a = colours[0];
        const Color& b = colours[1];
        GradientDef g;
        if (name.find("SPHERICAL") != std::string::npos) {
            g.kind = core::GradientKind::Radial;
            g.center = centre;
            g.rx = g.ry = std::max(len(sub({x1, y1}, centre)), 1e-6);
            g.stops = name.find("INV") == 0 ? std::vector<core::GradientStop>{{0, b}, {1, a}}
                                            : std::vector<core::GradientStop>{{0, a}, {1, b}};
        }
        else {
            const double angle = r.num(460);
            const Vec2 dir{std::cos(angle), std::sin(angle)};
            double lo = 1e300, hi = -1e300;
            for (const Vec2& corner : {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x1, y1}, Vec2{x0, y1}}) {
                lo = std::min(lo, dot(sub(corner, centre), dir));
                hi = std::max(hi, dot(sub(corner, centre), dir));
            }
            g.start = add(centre, mul(dir, lo));
            g.end = add(centre, mul(dir, hi));
            if (len(sub(g.end, g.start)) < 1e-9) {
                return;
            }
            const bool inverted = name.find("INV") == 0;
            if (name.find("CYLINDER") != std::string::npos) {  // (across and back)
                g.stops = inverted ? std::vector<core::GradientStop>{{0, b}, {0.5, a}, {1, b}}
                                   : std::vector<core::GradientStop>{{0, a}, {0.5, b}, {1, a}};
            }
            else {
                g.stops = inverted ? std::vector<core::GradientStop>{{0, b}, {1, a}} : std::vector<core::GradientStop>{{0, a}, {1, b}};
            }
        }
        note("DXF gradient hatches came in as gradients close to how they look");
        shape.style.fill = colours[0];
        shape.style.fillGradient = g;
    }

    void addShape(std::vector<Contour>& contours, const std::string& layer, const LayerInfo& info, Color col,
                  const Record& r)
    {
        if (contours.empty()) {
            return;
        }
        Shape s;
        s.contours = std::move(contours);
        s.layer = layer;
        // Our own exports: GEOMETRY (curves without a stroke) and CONSTRUCTION.
        const std::string up = upper(layer);
        if (up == "CONSTRUCTION") {
            s.style.construction = true;
        }
        else if (up != "GEOMETRY") {
            s.style.stroke = col;
            int lw = static_cast<int>(r.num(370, -1));
            if (lw == -1) {
                lw = info.lineweight;
            }
            s.style.strokeWidth = lw >= 0 ? lw / 100.0 : 0.25;
            const std::string* lt = r.get(6);
            s.style.dash = dashFor(lt && upper(*lt) != "BYLAYER" ? *lt : info.linetype);
        }
        if (std::ranges::find(m_out.layers, layer) == m_out.layers.end()) {
            m_out.layers.push_back(layer);
        }
        m_out.shapes.push_back(std::move(s));
    }

    std::vector<std::pair<int, std::string>> m_pairs;
    std::optional<double> m_mmPerUnit;
    QString m_folder;  // the file's own (for pictures it links to)
    std::map<std::string, std::string> m_imageFiles;  // IMAGEDEF handle -> the picture's path
    std::map<std::string, std::string> m_textStyles;  // STYLE name (upper case) -> font family
    int m_insunits = 0;
    std::map<std::string, LayerInfo> m_layers;
    std::map<std::string, Block> m_blocks;
    std::vector<Record> m_entities;
    Drawing m_out;
};
}  // namespace

Drawing readDxf(const QByteArray& data, std::optional<double> mmPerUnit, const QString& folder)
{
    DxfReader reader(data, mmPerUnit, folder);
    return reader.read();
}

Drawing read(const QString& path, double pxPerInch, std::optional<double> mmPerUnit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw core::SketchError("can't open " + path.toStdString());
    }
    const QByteArray data = file.readAll();
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("dxf")) {
        return readDxf(data, mmPerUnit, QFileInfo(path).absolutePath());
    }
    if (suffix == QStringLiteral("svg")) {
        return readSvg(data, pxPerInch, QFileInfo(path).absolutePath());
    }
    throw core::SketchError("only SVG and DXF files can be imported");
}

// =============================================================================================
// Building
// =============================================================================================

namespace
{
// Points by place: ends within `tol` of each other are one point (so outlines close).
class PointPool
{
public:
    PointPool(core::Sketch& sketch, double tol)
        : m_sketch(sketch)
        , m_tol(tol)
    {}

    Id at(Vec2 p)
    {
        const long long kx = std::llround(p.x / m_tol), ky = std::llround(p.y / m_tol);
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                const auto it = m_grid.find(key(kx + dx, ky + dy));
                if (it == m_grid.end()) {
                    continue;
                }
                for (const auto& [q, id] : it->second) {
                    if (len(sub(p, q)) <= m_tol) {
                        return id;
                    }
                }
            }
        }
        const Id id = m_sketch.addPoint(p.x, p.y);
        m_grid[key(kx, ky)].push_back({p, id});
        return id;
    }
    Id fresh(Vec2 p) { return m_sketch.addPoint(p.x, p.y); }

private:
    static long long key(long long x, long long y) { return x * 1000003LL ^ y; }
    core::Sketch& m_sketch;
    double m_tol;
    std::unordered_map<long long, std::vector<std::pair<Vec2, Id>>> m_grid;
};

Vec2 tangentAtEnd(const Segment& s)
{
    return len(sub(s.p[3], s.p[2])) > 1e-12 ? sub(s.p[3], s.p[2]) : sub(s.p[3], s.p[1]);
}

Vec2 tangentAtStart(const Segment& s)
{
    return len(sub(s.p[1], s.p[0])) > 1e-12 ? sub(s.p[1], s.p[0]) : sub(s.p[2], s.p[0]);
}

bool smoothJoin(const Segment& a, const Segment& b)
{
    const Vec2 u = tangentAtEnd(a), v = tangentAtStart(b);
    const double lu = len(u), lv = len(v);
    return lu > 0 && lv > 0 && dot(u, v) / (lu * lv) > std::cos(10.0 * kPi / 180);
}

bool straightCubic(const Segment& s)
{
    const Vec2 d = sub(s.p[3], s.p[0]);
    const double l = len(d);
    if (l < 1e-12) {
        return false;
    }
    const double tol = 1e-9 * std::max(1.0, l);
    return std::abs(cross(d, sub(s.p[1], s.p[0]))) / l < tol && std::abs(cross(d, sub(s.p[2], s.p[0]))) / l < tol;
}

class Builder
{
public:
    Builder(core::Sketch& sketch, PointPool& pool, Vec2 shift, Curves curves)
        : m_s(sketch)
        , m_pool(pool)
        , m_shift(shift)
        , m_curves(curves)
    {}

    std::vector<Id> contour(const Contour& c)
    {
        std::vector<Id> out;
        const auto& segs = c.segments;
        size_t i = 0;
        while (i < segs.size()) {
            if (segs[i].kind == Segment::Kind::Cubic && m_curves == Curves::Smooth) {
                // A run of curves joining smoothly: one fit spline through points along them.
                size_t j = i;
                while (j + 1 < segs.size() && segs[j + 1].kind == Segment::Kind::Cubic
                       && smoothJoin(segs[j], segs[j + 1])) {
                    ++j;
                }
                const bool whole = c.closed && i == 0 && j + 1 == segs.size() && smoothJoin(segs[j], segs[0]);
                fitRun(segs, i, j, whole, out);
                i = j + 1;
                continue;
            }
            single(segs[i], out);
            ++i;
        }
        return out;
    }

    // A gradient, with the geometry that places it (its two points; or a construction circle or
    // ellipse, and a focal point), which is added to `made` (to be held in the layer's group).
    core::Gradient gradient(const GradientDef& d, std::vector<Id>& made)
    {
        core::Gradient g;
        g.kind = d.kind;
        g.spread = d.spread;
        g.stops = d.stops;
        if (d.kind == core::GradientKind::Linear) {
            g.start = m_pool.fresh(at(d.start));
            g.end = m_pool.fresh(at(d.end));
            made.insert(made.end(), {g.start, g.end});
            return g;
        }
        const Id c = m_pool.fresh(at(d.center));
        made.push_back(c);
        if (std::abs(d.rx - d.ry) <= 1e-9 * std::max(d.rx, d.ry)) {
            g.shape = m_s.addCircle(c, d.rx);
        }
        else {
            const Vec2 u{std::cos(d.phi), std::sin(d.phi)};
            const Id major = m_pool.fresh(at(add(d.center, mul(u, d.rx))));
            const Id minor = m_pool.fresh(at(add(d.center, mul(Vec2{-u.y, u.x}, d.ry))));
            made.insert(made.end(), {major, minor});
            g.shape = m_s.addEllipse(c, major, minor);
        }
        m_s.entity(g.shape).construction = true;
        made.push_back(g.shape);
        if (d.focal) {
            g.focal = m_pool.fresh(at(*d.focal));
            made.push_back(g.focal);
        }
        return g;
    }

private:
    Vec2 at(Vec2 p) const { return add(p, m_shift); }

    void single(const Segment& s, std::vector<Id>& out)
    {
        switch (s.kind) {
            case Segment::Kind::Line: {
                const Id a = m_pool.at(at(s.p[0])), b = m_pool.at(at(s.p[1]));
                if (a != b) {
                    out.push_back(once({0, std::min(a, b), std::max(a, b), 0}, [&] { return m_s.addLine(a, b); }));
                }
                break;
            }
            case Segment::Kind::Cubic: {
                if (straightCubic(s)) {
                    single(line(s.p[0], s.p[3]), out);
                    break;
                }
                const Id a = m_pool.at(at(s.p[0]));
                Id d = m_pool.at(at(s.p[3]));
                if (d == a) {
                    d = m_pool.fresh(at(s.p[3]));  // (a loop: its ends are distinct points)
                }
                core::Spline sp;
                sp.kind = core::SplineKind::Control;
                sp.points = {a, m_pool.fresh(at(s.p[1])), m_pool.fresh(at(s.p[2])), d};
                out.push_back(m_s.addSpline(sp));
                break;
            }
            case Segment::Kind::Ellipse: {
                if (s.rx < 1e-9 || s.ry < 1e-9) {
                    break;
                }
                const bool round = std::abs(s.rx - s.ry) <= 1e-9 * std::max(s.rx, s.ry);
                const Id c = m_pool.at(at(s.c));
                const Vec2 u{std::cos(s.phi), std::sin(s.phi)};
                auto axes = [&] {
                    return std::pair{m_pool.fresh(at(add(s.c, mul(u, s.rx)))),
                                     m_pool.fresh(at(add(s.c, mul(Vec2{-u.y, u.x}, s.ry))))};
                };
                if (s.whole()) {
                    if (round) {
                        out.push_back(once({1, c, 0, std::llround(s.rx * 1e6)}, [&] { return m_s.addCircle(c, s.rx); }));
                    }
                    else {
                        const auto [major, minor] = axes();
                        out.push_back(m_s.addEllipse(c, major, minor));
                    }
                    break;
                }
                if (std::abs(s.dt) < 1e-9) {
                    break;
                }
                Id a = m_pool.at(at(s.start())), b = m_pool.at(at(s.end()));
                if (a == b || a == c || b == c) {
                    break;
                }
                if (s.dt < 0) {
                    std::swap(a, b);  // (ours run counter-clockwise)
                }
                if (round) {
                    out.push_back(once({2, c, a, static_cast<long long>(b)}, [&] { return m_s.addArc(c, a, b); }));
                }
                else {
                    const auto [major, minor] = axes();
                    out.push_back(m_s.addEllipseArc(c, major, minor, a, b));
                }
                break;
            }
        }
    }

    void fitRun(const std::vector<Segment>& segs, size_t i, size_t j, bool whole, std::vector<Id>& out)
    {
        std::vector<Vec2> pts{segs[i].p[0]};
        for (size_t k = i; k <= j; ++k) {
            const std::vector<Vec2> along = segs[k].sample(3);  // (start, thirds, end)
            pts.insert(pts.end(), along.begin() + 1, along.end());
        }
        std::vector<Vec2> clean;
        for (const Vec2& p : pts) {
            if (clean.empty() || len(sub(p, clean.back())) > 1e-9) {
                clean.push_back(p);
            }
        }
        if (whole && clean.size() > 3 && len(sub(clean.front(), clean.back())) < 1e-6) {
            clean.pop_back();
            core::Spline sp;
            sp.kind = core::SplineKind::Fit;
            sp.closed = true;
            for (const Vec2& p : clean) {
                sp.points.push_back(m_pool.fresh(at(p)));
            }
            out.push_back(m_s.addSpline(sp));
            return;
        }
        if (clean.size() < 3) {
            single(line(segs[i].p[0], segs[j].p[3]), out);
            return;
        }
        core::Spline sp;
        sp.kind = core::SplineKind::Fit;
        for (size_t k = 0; k < clean.size(); ++k) {
            const bool end = k == 0 || k + 1 == clean.size();
            sp.points.push_back(end ? m_pool.at(at(clean[k])) : m_pool.fresh(at(clean[k])));
        }
        if (sp.points.front() == sp.points.back()) {
            sp.points.back() = m_pool.fresh(at(clean.back()));
        }
        out.push_back(m_s.addSpline(sp));
    }

    // A curve the file has twice (a hatch's boundary over its outline, say): made once.
    template <typename Make>
    Id once(const std::array<long long, 4>& key, const Make& make)
    {
        const auto [it, fresh] = m_made.try_emplace(key, kNoId);
        if (fresh) {
            it->second = make();
        }
        return it->second;
    }

    core::Sketch& m_s;
    PointPool& m_pool;
    Vec2 m_shift;
    Curves m_curves;
    std::map<std::array<long long, 4>, Id> m_made;  // lines, circles and arcs made, by their points
};
}  // namespace

Report build(core::Sketch& sketch, const Drawing& drawing, const Options& o)
{
    Report report;
    report.skipped = drawing.skipped;
    report.notes = drawing.notes;
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (!drawing.bounds(bx0, by0, bx1, by1)) {
        throw core::SketchError("there's nothing in that file to import");
    }

    // Where it goes.
    Vec2 shift{0, 0};
    switch (o.placement) {
        case Placement::AsInFile: break;
        case Placement::Center: shift = {o.at.x - (bx0 + bx1) / 2, o.at.y - (by0 + by1) / 2}; break;
        case Placement::Page:
            if (const auto& page = sketch.page()) {
                const std::array<double, 4> r = page->rect();
                const double fx = drawing.canvas ? (*drawing.canvas)[0] : bx0;
                const double fy = drawing.canvas ? (*drawing.canvas)[1] : by0;
                shift = {r[0] - fx, r[1] - fy};
            }
            break;
    }

    // The file's layers (in order) and their shapes.
    std::vector<std::string> fileLayers;
    std::map<std::string, std::vector<const Shape*>> byLayer;
    for (const Shape& s : drawing.shapes) {
        const std::string key = o.keepLayers ? s.layer : std::string();
        if (!byLayer.contains(key)) {
            fileLayers.push_back(key);
        }
        byLayer[key].push_back(&s);
    }
    std::map<std::string, std::vector<const TextDef*>> textsByLayer;
    auto alsoLayer = [&](const std::string& fileLayer) {  // (a layer of only pictures or texts is a layer too)
        const std::string key = o.keepLayers ? fileLayer : std::string();
        if (!byLayer.contains(key)) {
            fileLayers.push_back(key);
            byLayer[key];
        }
        return key;
    };
    for (const ImageDef& im : drawing.images) {
        alsoLayer(im.layer);
    }
    for (const TextDef& td : drawing.texts) {
        textsByLayer[alsoLayer(td.layer)].push_back(&td);
    }
    if (o.keepLayers) {
        std::vector<std::string> ordered;  // (the file's own order where it gives one)
        for (const std::string& l : drawing.layers) {
            if (byLayer.contains(l)) {
                ordered.push_back(l);
            }
        }
        for (const std::string& l : fileLayers) {
            if (std::ranges::find(ordered, l) == ordered.end()) {
                ordered.push_back(l);
            }
        }
        fileLayers = ordered;
    }

    // The sublayers they go into.
    const std::string base = o.name.empty() ? std::string("Imported") : o.name;
    auto label = [&](const std::string& fileLayer) {
        return fileLayer.empty() || fileLayers.size() == 1 ? base : fileLayer;
    };
    std::map<std::string, Id> target;
    switch (o.into) {
        case Into::Sublayer:
            for (const std::string& l : fileLayers) {
                target[l] = sketch.activeSublayer();
            }
            break;
        case Into::NewSublayer:
            for (const std::string& l : fileLayers) {
                const Id sub = sketch.addSublayer(Editor::uniqueSublayerName(sketch, QString::fromStdString(label(l))).toStdString());
                sketch.setActiveSublayer(sub);
                target[l] = sub;
            }
            break;
        case Into::NewLayer: {
            const std::string name = Editor::uniqueLayerName(sketch, QString::fromStdString(base)).toStdString();
            const Id layer = sketch.addLayer(name, label(fileLayers.front()));
            sketch.setActiveLayer(layer);
            target[fileLayers.front()] = sketch.activeSublayer();
            for (size_t k = 1; k < fileLayers.size(); ++k) {
                const Id sub = sketch.addSublayer(
                    Editor::uniqueSublayerName(sketch, QString::fromStdString(label(fileLayers[k]))).toStdString());
                sketch.setActiveSublayer(sub);
                target[fileLayers[k]] = sub;
            }
            break;
        }
    }

    // Geometry, file layer by file layer: each a group of its own.
    struct Filled
    {
        const Shape* shape;
        std::vector<std::vector<Vec2>> rings;
        double x0, y0, x1, y1;
    };
    std::map<Id, std::vector<Filled>> fillsByLayer;  // (by our layer: fills are found per layer)
    std::map<const Shape*, core::Gradient> fillGradients;
    std::set<Id> imported;
    std::vector<Id> sublayersUsed;
    const double size = std::max({bx1 - bx0, by1 - by0, 1.0});
    for (const std::string& l : fileLayers) {
        sketch.setActiveSublayer(target[l]);
        PointPool pool(sketch, std::max(1e-6, 1e-7 * size));
        Builder builder(sketch, pool, shift, o.curves);
        std::vector<Id> made;
        // Gradients placed so far in this layer: shapes painted alike share one (and its geometry).
        std::vector<std::pair<GradientDef, core::Gradient>> placed;
        auto gradientOf = [&](const GradientDef& d) {
            for (const auto& [def, g] : placed) {
                if (def == d) {
                    return g;
                }
            }
            placed.emplace_back(d, builder.gradient(d, made));
            ++report.gradients;
            return placed.back().second;
        };
        for (const Shape* shape : byLayer[l]) {
            std::vector<Id> curves;
            std::set<Id> lineless;  // (a clip's edge across a fill: no line of its own)
            for (const Contour& c : shape->contours) {
                const std::vector<Id> ids = builder.contour(c);
                curves.insert(curves.end(), ids.begin(), ids.end());
                if (c.lineless) {
                    lineless.insert(ids.begin(), ids.end());
                }
            }
            const Style& st = shape->style;
            const core::Gradient strokeGradient =
                st.strokeGradient && st.stroke && !st.construction && !curves.empty() ? gradientOf(*st.strokeGradient) : core::Gradient{};
            if (st.fillGradient && st.fill && !st.construction) {
                fillGradients[shape] = gradientOf(*st.fillGradient);
            }
            for (Id id : curves) {
                if (lineless.contains(id)) {
                    if (!st.construction && !sketch.stroke(id)) {
                        core::StrokeStyle none;
                        none.width = core::StrokeStyle::kNone;
                        sketch.setStroke(id, none);
                    }
                    continue;
                }
                if (st.construction) {
                    sketch.entity(id).construction = true;
                }
                else if (st.stroke) {
                    core::StrokeStyle stroke;
                    stroke.color = *st.stroke;
                    stroke.width = st.strokeWidth;
                    stroke.dash = st.dash;
                    stroke.gradient = strokeGradient;
                    sketch.setStroke(id, stroke);
                }
                else if (st.fill && !sketch.stroke(id)) {
                    core::StrokeStyle none;
                    none.width = core::StrokeStyle::kNone;  // (a fill's outline: no line of its own)
                    sketch.setStroke(id, none);
                }
            }
            imported.insert(curves.begin(), curves.end());
            made.insert(made.end(), curves.begin(), curves.end());
            report.curves += static_cast<int>(curves.size());
            if (st.fill && !st.construction) {
                // Where it's filled: its outline (as it was before any clip cut it).
                Filled f{shape, {}, 1e300, 1e300, -1e300, -1e300};
                for (std::vector<Vec2> ring : shape->fillArea ? shape->fillArea->rings : ringsOf(shape->contours)) {
                    for (Vec2& q : ring) {
                        q = add(q, shift);
                        f.x0 = std::min(f.x0, q.x);
                        f.y0 = std::min(f.y0, q.y);
                        f.x1 = std::max(f.x1, q.x);
                        f.y1 = std::max(f.y1, q.y);
                    }
                    f.rings.push_back(std::move(ring));
                }
                if (!f.rings.empty()) {
                    fillsByLayer[sketch.layerOfSublayer(target[l])].push_back(std::move(f));
                }
            }
        }
        // Texts, on points of their own held in the group with the rest.
        for (const TextDef* td : textsByLayer[l]) {
            core::Text text = td->text;
            text.sublayer = target[l];
            if (td->path) {
                // Along its path: construction curves, in the way the path runs.
                std::vector<Id> chain = builder.contour(*td->path);
                if (chain.empty()) {
                    continue;
                }
                for (Id c : chain) {
                    sketch.entity(c).construction = true;
                }
                made.insert(made.end(), chain.begin(), chain.end());
                text.path = chain.front();
                text.pathMore.assign(chain.begin() + 1, chain.end());
                text.anchor = sketch.definingPoints(text.path).front();
                // (along one arc, the text runs clockwise unless flipped: the path's way, then)
                const core::EntityKind kind = sketch.entity(text.path).kind();
                if (chain.size() == 1 && (kind == core::EntityKind::Arc || kind == core::EntityKind::EllipseArc)) {
                    const Id start = kind == core::EntityKind::Arc ? sketch.arc(text.path).start : sketch.ellipseArc(text.path).start;
                    const Vec2 from = add(td->path->segments.front().start(), shift);
                    const core::Point& s0 = sketch.point(start);
                    text.flipH = std::hypot(s0.x - from.x, s0.y - from.y) < 1e-6 * std::max(1.0, size);  // (it runs anticlockwise)
                }
                textshapes::update(text, &sketch);
                sketch.addText(text);
                ++report.texts;
                continue;
            }
            const Vec2 p = add(td->at, shift);
            text.anchor = sketch.addPoint(p.x, p.y);
            textshapes::update(text, &sketch);
            sketch.addText(text);
            made.push_back(text.anchor);
            ++report.texts;
        }
        if (!made.empty()) {
            try {
                core::group::create(sketch, made);
                ++report.groups;
            }
            catch (const core::SketchError&) {
                // (a single point's worth of geometry: nothing to hold together)
            }
        }
        if (std::ranges::find(sublayersUsed, target[l]) == sublayersUsed.end()) {
            sublayersUsed.push_back(target[l]);
            report.sublayers.push_back(sketch.sublayer(target[l]).name);
        }
    }

    // Pictures: reference images on the layers their file layers went to.
    for (const ImageDef& def : drawing.images) {
        core::Image im = def.image;
        im.x += shift.x;
        im.y += shift.y;
        im.layer = sketch.layerOfSublayer(target[o.keepLayers ? def.layer : std::string()]);
        sketch.addImage(im);
        ++report.images;
    }

    // Fills: each closed area made only of imported curves takes the colour of the last filled
    // shape over it (by that shape's fill rule).
    for (auto& [layer, shapes] : fillsByLayer) {
        const core::regions::Arrangement arrangement(sketch, layer);
        for (const core::regions::Region& region : arrangement.regions()) {
            if (region.boundary.empty()
                || !std::ranges::all_of(region.boundary, [&](Id c) { return imported.contains(c); })) {
                continue;
            }
            const Vec2 p = region.interiorPoint();
            const Filled* top = nullptr;
            for (const Filled& f : shapes) {
                if (p.x >= f.x0 && p.x <= f.x1 && p.y >= f.y0 && p.y <= f.y1
                    && pointInShape(f.rings, p, f.shape->style.evenOdd)
                    && std::ranges::all_of(f.shape->clips, [&](const std::vector<Area>& clip) {  // (and inside its clips)
                           return std::ranges::any_of(clip, [&](const Area& a) { return inArea(a, sub(p, shift)); });
                       })) {
                    top = &f;
                }
            }
            if (!top) {
                continue;
            }
            core::Fill fill;
            fill.color = *top->shape->style.fill;  // (a gradient's: its average, should it lose its geometry)
            if (const auto it = fillGradients.find(top->shape); it != fillGradients.end()) {
                fill.gradient = it->second;
            }
            if (top->shape->style.hatch) {
                fill.hatch = *top->shape->style.hatch;
                ++report.hatches;
            }
            fill.seed = core::Point{p.x, p.y};
            fill.boundary = region.boundary;
            fill.layer = layer;
            sketch.addFill(fill);
            ++report.fills;
        }
    }
    return report;
}

}  // namespace importer

