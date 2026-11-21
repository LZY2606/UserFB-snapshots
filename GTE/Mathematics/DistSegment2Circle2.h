// David Eberly, Geometric Tools, Redmond WA 98052
// Copyright (c) 1998-2026
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt
// https://www.geometrictools.com/License/Boost/LICENSE_1_0.txt
// File Version: 8.0.2026.09.19

#pragma once

// Compute the distance between a segment and a circle in 2D. The circle is
// considered to be a curve, not a solid disk.
//
// The segment has endpoints P0 and P1 and is parameterized by
// P0 + t * (P1 - P0). The t-value satisfies 0 <= t <= 1.
//
// The circle is C + r * U(s), where C is the center, r > 0 is the radius,
// and U(s) = (cos(s), sin(s)) for s in [0,2*pi).
//
// The number of pairs of closest points is lcResult.numClosestPairs which is
// 1 or 2. If lcResult.numClosestPairs is 1, lcResult.parameter[0] is the
// segment t-value for its closest point lcResult.closest[0][0]. The circle
// closest point is lcResult.closest[0][1]. If lcResult.numClosestPairs is 2,
// lcResult.parameter[0] and lcResult.parameter[1] are the segment t-values
// for its closest points lcResult.closest[0][0] and lcResult.closest[1][0].
// The circle closest points are lcResult.closest[0][1] and
// lcResult.closest[1][1].

#include <Mathematics/Logger.h>
#include <Mathematics/DistLine2Circle2.h>
#include <Mathematics/DistPoint2Circle2.h>
#include <Mathematics/Segment.h>

namespace gte
{
    template <typename T>
    class DCPQuery<T, Segment2<T>, Circle2<T>>
    {
    public:
        using LCQuery = DCPQuery<T, Line2<T>, Circle2<T>>;
        using Result = typename LCQuery::Result;

        Result operator()(Segment2<T> const& segment, Circle2<T> const& circle)
        {
            LogAssert(
                segment.p[0] != segment.p[1] &&
                circle.radius > static_cast<T>(0),
                "Invalid input.");

            // Execute the query for line-circle.
            T zero = static_cast<T>(0);
            T one = static_cast<T>(1);
            Line2<T> line(segment.p[0], segment.p[1] - segment.p[0]);
            Result lcResult = LCQuery{}(line, circle);

            T const& s0 = zero;
            T const& s1 = one;
            T const& t0 = lcResult.parameter[0];
            T const& t1 = lcResult.parameter[1];

            // Restrict the analysis to segment-circle. This is equivalent to
            // computing the intersection of [s0,s1] and [t0,t1].
            if (lcResult.numClosestPairs == 2)
            {
                if (s0 < t0)
                {
                    if (s1 <= t0)
                    {
                        // When s1 < t0, the segment is strictly outside the
                        // circle, to the left of the circle. When s1 = t0,
                        // the segment is outside the circle except for the
                        // right-most endpoint of the segment. The closest
                        // point is segment.p[1].
                        Update(segment.p[1], one, circle, lcResult);
                    }
                    else if (s1 > t0)
                    {
                        if (s1 < t1)
                        {
                            // The segment and circle intersect at the
                            // left-most line-circle intersection point.
                            Update(0, lcResult);
                        }
                        else // s1 >= t1
                        {
                            // The segment and circle intersect at 2 points,
                            // the left-most and right-most line-circle
                            // intersection points. The incoming segment
                            // strictly contains the subsegment of the line
                            // that is contained inside the circle.
                            //
                            // NOTE. See the "NOTE" written later.
                        }
                    }
                }
                else if (t1 < s1)
                {
                    if (t1 <= s0)
                    {
                        // When t1 < s0, the segment is strictly outside the
                        // circle, to the right of the circle. When t1 = s0,
                        // the segment is outside the circle except for the
                        // left-most endpoint of the segment. The closest
                        // point is segment.p[0].
                        Update(segment.p[0], zero, circle, lcResult);
                    }
                    else if (t1 > s0)
                    {
                        if (t0 < s0)
                        {
                            // The segment and circle intersect at the
                            // right-most line-circle intersection point.
                            Update(1, lcResult);
                        }
                        else // t0 >= s0
                        {
                            // The segment and circle intersect at 2 points,
                            // the left-most and right-most line-circle
                            // intersection points. The incoming segment
                            // strictly contains the subsegment of the line
                            // that is contained inside the circle.
                            //
                            // NOTE. This block is not reached. By symmetry
                            // of the interval processing, the block that is
                            // reached when s0 <= t0 < t1 < s1, which is the
                            // Boolean expression for the previous "NOTE".
                        }
                    }
                }
                else // t0 <= s0 < s1 <= t1
                {
                    // The segment is inside the circle.
                    if (t0 < s0)
                    {
                        if (s1 < t1) // t0 < s0 < s1 < t1
                        {
                            // The segment is strictly inside the circle.
                            // Determine which endpoint is closest to the
                            // circle, possibly both endpoints equidistant
                            // from the circle.
                            Update(segment, circle, lcResult);
                        }
                        else // t0 < s0 < s1 = t1
                        {
                            // The segment point closest to the circle is
                            // segment.p[1] with distance 0.
                            Update(segment.p[1], one, circle, lcResult);
                        }
                    }
                    else
                    {
                        if (s1 < t1) // t0 = s0 < s1 < t1
                        {
                            // The segment point closest to the circle is
                            // segment.p[0] with distance 0.
                            Update(segment.p[0], zero, circle, lcResult);
                        }
                        else // t0 = s0 < s1 = t1
                        {
                            // The segment endpoints are equidistant to the
                            // circle with distance 0.
                        }
                    }
                }

            }
            else // lcResult.numClosestPairs = 1
            {
                // The line does not intersect the circle or is tangent to the
                // circle.
                if (lcResult.parameter[0] < zero)
                {
                    // The segment point closest to the circle is segment.p[0].
                    Update(segment.p[0], zero, circle, lcResult);
                }
                else if (lcResult.parameter[0] > one)
                {
                    // The segment point closest to the circle is segment.p[1].
                    Update(segment.p[1], one, circle, lcResult);
                }
                else
                {
                    // The segment point closest to the circle is the line
                    // point closest to the circle.
                }
            }

            return lcResult;
        }

    private:
        // Updates for the line-circle lcResult to the segment-circle lcResult.
        using PCQuery = DCPQuery<T, Vector2<T>, Circle2<T>>;
        using PCResult = typename PCQuery::Result;

        static void Update(Segment2<T> const& segment,
            Circle2<T> const& circle, Result& lcResult)
        {
            T const zero = static_cast<T>(0);
            T const one = static_cast<T>(1);

            Result result0{}, result1{};
            Update(segment.p[0], zero, circle, result0);
            Update(segment.p[1], one, circle, result1);
            if (result0.distance < result1.distance)
            {
                lcResult = result0;
            }
            else if (result0.distance > result1.distance)
            {
                lcResult = result1;
            }
            else
            {
                lcResult.distance = result0.distance;
                lcResult.sqrDistance = result0.sqrDistance;
                lcResult.numClosestPairs = 2;
                lcResult.parameter[0] = zero;
                lcResult.parameter[1] = one;
                lcResult.closest[0][0] = result0.closest[0][0];
                lcResult.closest[0][1] = result0.closest[0][1];
                lcResult.closest[1][0] = result1.closest[0][0];
                lcResult.closest[1][1] = result1.closest[0][1];
            }
        }

        static void Update(Vector2<T> const& endpoint, T const& parameter,
            Circle2<T> const& circle, Result& lcResult)
        {
            T const zero = static_cast<T>(0);
            auto pcResult = PCQuery{}(endpoint, circle);
            lcResult.distance = pcResult.distance;
            lcResult.sqrDistance = pcResult.sqrDistance;
            lcResult.numClosestPairs = 1;
            lcResult.parameter[0] = parameter;
            lcResult.parameter[1] = zero;
            lcResult.closest[0][0] = pcResult.closest[0];
            lcResult.closest[0][1] = pcResult.closest[1];
            lcResult.closest[1][0] = { zero, zero };
            lcResult.closest[1][1] = { zero, zero };
        }

        static void Update(std::size_t i, Result& lcResult)
        {
            T const zero = static_cast<T>(0);
            lcResult.distance = zero;
            lcResult.sqrDistance = zero;
            lcResult.numClosestPairs = 1;
            lcResult.parameter[0] = lcResult.parameter[i];
            lcResult.parameter[1] = zero;
            lcResult.closest[0][0] = lcResult.closest[i][0];
            lcResult.closest[0][1] = lcResult.closest[i][1];
            lcResult.closest[1][0] = { zero, zero };
            lcResult.closest[1][1] = { zero, zero };
        }
    };
}
