// Smoke tests for the vendored PlaneGCS solver: proves it builds, solves, and reports
// degrees of freedom and conflicts the way the sketch layer will rely on.

#include <planegcs/GCS.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{

// Two points with x/y params owned by the test; tags must be >= 1 for conflict reporting.
struct TwoPoints
{
    double x1 = 0.0, y1 = 0.0;
    double x2 = 10.0, y2 = 3.0;
    double fixX = 0.0, fixY = 0.0;
    double distance = 50.0;

    GCS::Point p1{&x1, &y1};
    GCS::Point p2{&x2, &y2};
    GCS::System sys;

    GCS::VEC_pD params() { return {&x1, &y1, &x2, &y2}; }

    int solve()
    {
        auto unknowns = params();
        sys.declareUnknowns(unknowns);
        sys.initSolution(GCS::DogLeg);
        const auto status = sys.solve(GCS::DogLeg);
        if (status == GCS::SolveStatus::Success || status == GCS::SolveStatus::Converged) {
            sys.applySolution();
        }
        return static_cast<int>(status);
    }
};

}  // namespace

TEST_CASE("Fixed point, horizontal and distance fully constrain a segment", "[planegcs]")
{
    TwoPoints t;
    t.sys.addConstraintCoordinateX(t.p1, &t.fixX, 1);
    t.sys.addConstraintCoordinateY(t.p1, &t.fixY, 2);
    t.sys.addConstraintHorizontal(t.p1, t.p2, 3);
    t.sys.addConstraintP2PDistance(t.p1, t.p2, &t.distance, 4);

    REQUIRE(t.solve() == static_cast<int>(GCS::SolveStatus::Success));
    CHECK(t.sys.dofsNumber() == 0);
    CHECK(t.x1 == Catch::Approx(0.0).margin(1e-9));
    CHECK(t.y1 == Catch::Approx(0.0).margin(1e-9));
    CHECK(t.x2 == Catch::Approx(50.0).margin(1e-9));  // stays on the side it started on
    CHECK(t.y2 == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("Distance alone leaves three degrees of freedom", "[planegcs]")
{
    TwoPoints t;
    t.sys.addConstraintP2PDistance(t.p1, t.p2, &t.distance, 1);

    REQUIRE(t.solve() == static_cast<int>(GCS::SolveStatus::Success));
    CHECK(t.sys.dofsNumber() == 3);
    const double dx = t.x2 - t.x1, dy = t.y2 - t.y1;
    CHECK(std::sqrt(dx * dx + dy * dy) == Catch::Approx(50.0).epsilon(1e-9));
}

TEST_CASE("Horizontal plus vertical on a non-zero distance is reported as conflicting", "[planegcs]")
{
    TwoPoints t;
    t.sys.addConstraintHorizontal(t.p1, t.p2, 1);
    t.sys.addConstraintVertical(t.p1, t.p2, 2);
    t.sys.addConstraintP2PDistance(t.p1, t.p2, &t.distance, 3);

    t.solve();
    GCS::VEC_I conflicting;
    t.sys.getConflicting(conflicting);
    CHECK_FALSE(conflicting.empty());
}
