/********************************************************************************
* ReactPhysics3D physics library, http://www.reactphysics3d.com                 *
* Copyright (c) 2010-2016 Daniel Chappuis                                       *
*********************************************************************************
*                                                                               *
* This software is provided 'as-is', without any express or implied warranty.   *
* In no event will the authors be held liable for any damages arising from the  *
* use of this software.                                                         *
*                                                                               *
* Permission is granted to anyone to use this software for any purpose,         *
* including commercial applications, and to alter it and redistribute it        *
* freely, subject to the following restrictions:                                *
*                                                                               *
* 1. The origin of this software must not be misrepresented; you must not claim *
*    that you wrote the original software. If you use this software in a        *
*    product, an acknowledgment in the product documentation would be           *
*    appreciated but is not required.                                           *
*                                                                               *
* 2. Altered source versions must be plainly marked as such, and must not be    *
*    misrepresented as being the original software.                             *
*                                                                               *
* 3. This notice may not be removed or altered from any source distribution.    *
*                                                                               *
********************************************************************************/

// Libraries
#include "SATAlgorithm.h"
#include "constraint/ContactPoint.h"
#include "collision/PolyhedronMesh.h"
#include "collision/shapes/CapsuleShape.h"
#include "collision/shapes/SphereShape.h"
#include "engine/OverlappingPair.h"
#include "configuration.h"
#include "engine/Profiler.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cassert>

// We want to use the ReactPhysics3D namespace
using namespace reactphysics3d;

// Static variables initialization
const decimal SATAlgorithm::SAME_SEPARATING_AXIS_BIAS = decimal(0.001);

// Test collision between a sphere and a convex mesh
bool SATAlgorithm::testCollisionSphereVsConvexPolyhedron(const NarrowPhaseInfo* narrowPhaseInfo, ContactManifoldInfo& contactManifoldInfo) const {

    PROFILE("SATAlgorithm::testCollisionSphereVsConvexPolyhedron()");

    bool isSphereShape1 = narrowPhaseInfo->collisionShape1->getType() == CollisionShapeType::SPHERE;

    assert(narrowPhaseInfo->collisionShape1->getType() == CollisionShapeType::CONVEX_POLYHEDRON ||
           narrowPhaseInfo->collisionShape2->getType() == CollisionShapeType::CONVEX_POLYHEDRON);
    assert(narrowPhaseInfo->collisionShape1->getType() == CollisionShapeType::SPHERE ||
           narrowPhaseInfo->collisionShape2->getType() == CollisionShapeType::SPHERE);

    // Get the capsule collision shapes
    const SphereShape* sphere = static_cast<const SphereShape*>(isSphereShape1 ? narrowPhaseInfo->collisionShape1 : narrowPhaseInfo->collisionShape2);
    const ConvexPolyhedronShape* polyhedron = static_cast<const ConvexPolyhedronShape*>(isSphereShape1 ? narrowPhaseInfo->collisionShape2 : narrowPhaseInfo->collisionShape1);

    const Transform& sphereToWorldTransform = isSphereShape1 ? narrowPhaseInfo->shape1ToWorldTransform : narrowPhaseInfo->shape2ToWorldTransform;
    const Transform& polyhedronToWorldTransform = isSphereShape1 ? narrowPhaseInfo->shape2ToWorldTransform : narrowPhaseInfo->shape1ToWorldTransform;

    // Get the transform from sphere local-space to polyhedron local-space
    const Transform worldToPolyhedronTransform = polyhedronToWorldTransform.getInverse();
    const Transform sphereToPolyhedronSpaceTransform = worldToPolyhedronTransform * sphereToWorldTransform;

    // Transform the center of the sphere into the local-space of the convex polyhedron
    const Vector3 sphereCenter = sphereToPolyhedronSpaceTransform.getPosition();

    // Minimum penetration depth
    decimal minPenetrationDepth = DECIMAL_LARGEST;
    uint minFaceIndex = 0;

    // True if the shapes were overlapping in the previous frame and are
    // still overlapping on the same axis in this frame
    bool isTemporalCoherenceValid = false;

    LastFrameCollisionInfo& lastFrameInfo = narrowPhaseInfo->overlappingPair->getLastFrameCollisionInfo();

    // If the shapes are not triangles (no temporal coherence for triangle collision because we do not store previous
    // frame collision data per triangle)
    if (polyhedron->getType() != CollisionShapeType::TRIANGLE) {

        // If the last frame collision info is valid and was also using SAT algorithm
        if (lastFrameInfo.isValid && lastFrameInfo.wasUsingSAT) {

            // We perform temporal coherence, we check if there is still an overlapping along the previous minimum separating
            // axis. If it is the case, we directly report the collision without executing the whole SAT algorithm again. If
            // the shapes are still separated along this axis, we directly exit with no collision.

            // Compute the penetration depth of the shapes along the face normal direction
            decimal penetrationDepth = computePolyhedronFaceVsSpherePenetrationDepth(lastFrameInfo.satMinAxisFaceIndex, polyhedron,
                                                                                     sphere, sphereCenter);

            // If the previous axis is a separating axis
            if (penetrationDepth <= decimal(0.0)) {

                // Return no collision
                return false;
            }

            // The two shapes are overlapping as in the previous frame and on the same axis, therefore
            // we will skip the entire SAT algorithm because the minimum separating axis did not change
            isTemporalCoherenceValid = lastFrameInfo.wasColliding;

            if (isTemporalCoherenceValid) {

                minPenetrationDepth = penetrationDepth;
                minFaceIndex = lastFrameInfo.satMinAxisFaceIndex;
            }
        }
    }

    // We the shapes are still overlapping in the same axis as in
    // the previous frame, we skip the whole SAT algorithm
    if (!isTemporalCoherenceValid) {

        // For each face of the convex mesh
        for (uint f = 0; f < polyhedron->getNbFaces(); f++) {

            // Compute the penetration depth of the shapes along the face normal direction
            decimal penetrationDepth = computePolyhedronFaceVsSpherePenetrationDepth(f, polyhedron, sphere, sphereCenter);

            // If the penetration depth is negative, we have found a separating axis
            if (penetrationDepth <= decimal(0.0)) {

                lastFrameInfo.satMinAxisFaceIndex = f;

                return false;
            }

            // Check if we have found a new minimum penetration axis
            if (penetrationDepth < minPenetrationDepth) {
                minPenetrationDepth = penetrationDepth;
                minFaceIndex = f;
            }
        }
    }

    const Vector3 minFaceNormal = polyhedron->getFaceNormal(minFaceIndex);
    Vector3 normalWorld = -(polyhedronToWorldTransform.getOrientation() * minFaceNormal);
    const Vector3 contactPointSphereLocal = sphereToWorldTransform.getInverse() * normalWorld * sphere->getRadius();
    const Vector3 contactPointPolyhedronLocal = sphereCenter + minFaceNormal * (minPenetrationDepth - sphere->getRadius());

    if (!isSphereShape1) {
        normalWorld = -normalWorld;
    }

    // Create the contact info object
    contactManifoldInfo.addContactPoint(normalWorld, minPenetrationDepth,
                                        isSphereShape1 ? contactPointSphereLocal : contactPointPolyhedronLocal,
                                        isSphereShape1 ? contactPointPolyhedronLocal : contactPointSphereLocal);

    lastFrameInfo.satMinAxisFaceIndex = minFaceIndex;

    return true;
}

// Compute the penetration depth between a face of the polyhedron and a sphere along the polyhedron face normal direction
decimal SATAlgorithm::computePolyhedronFaceVsSpherePenetrationDepth(uint faceIndex, const ConvexPolyhedronShape* polyhedron,
                                                                    const SphereShape* sphere, const Vector3& sphereCenter) const {

    // Get the face
    HalfEdgeStructure::Face face = polyhedron->getFace(faceIndex);

    // Get the face normal
    const Vector3 faceNormal = polyhedron->getFaceNormal(faceIndex);

    Vector3 sphereCenterToFacePoint = polyhedron->getVertexPosition(face.faceVertices[0]) - sphereCenter;
    decimal penetrationDepth = sphereCenterToFacePoint.dot(faceNormal) + sphere->getRadius();

    return penetrationDepth;
}

// Test collision between a capsule and a convex mesh
bool SATAlgorithm::testCollisionCapsuleVsConvexPolyhedron(const NarrowPhaseInfo* narrowPhaseInfo, ContactManifoldInfo& contactManifoldInfo) const {

    PROFILE("SATAlgorithm::testCollisionCapsuleVsConvexPolyhedron()");

    bool isCapsuleShape1 = narrowPhaseInfo->collisionShape1->getType() == CollisionShapeType::CAPSULE;

    assert(narrowPhaseInfo->collisionShape1->getType() == CollisionShapeType::CONVEX_POLYHEDRON ||
           narrowPhaseInfo->collisionShape2->getType() == CollisionShapeType::CONVEX_POLYHEDRON);
    assert(narrowPhaseInfo->collisionShape1->getType() == CollisionShapeType::CAPSULE ||
           narrowPhaseInfo->collisionShape2->getType() == CollisionShapeType::CAPSULE);

    // Get the collision shapes
    const CapsuleShape* capsuleShape = static_cast<const CapsuleShape*>(isCapsuleShape1 ? narrowPhaseInfo->collisionShape1 : narrowPhaseInfo->collisionShape2);
    const ConvexPolyhedronShape* polyhedron = static_cast<const ConvexPolyhedronShape*>(isCapsuleShape1 ? narrowPhaseInfo->collisionShape2 : narrowPhaseInfo->collisionShape1);

    const Transform capsuleToWorld = isCapsuleShape1 ? narrowPhaseInfo->shape1ToWorldTransform : narrowPhaseInfo->shape2ToWorldTransform;
    const Transform polyhedronToWorld = isCapsuleShape1 ? narrowPhaseInfo->shape2ToWorldTransform : narrowPhaseInfo->shape1ToWorldTransform;

    const Transform polyhedronToCapsuleTransform = capsuleToWorld.getInverse() * polyhedronToWorld;

    // Compute the end-points of the inner segment of the capsule
    const Vector3 capsuleSegA(0, -capsuleShape->getHeight() * decimal(0.5), 0);
    const Vector3 capsuleSegB(0, capsuleShape->getHeight() * decimal(0.5), 0);
    const Vector3 capsuleSegmentAxis = capsuleSegB - capsuleSegA;

    // Minimum penetration depth
    decimal minPenetrationDepth = DECIMAL_LARGEST;
    uint minFaceIndex = 0;
    uint minEdgeIndex = 0;
    bool isMinPenetrationFaceNormal = false;
    Vector3 separatingAxisCapsuleSpace;
    Vector3 separatingPolyhedronEdgeVertex1;
    Vector3 separatingPolyhedronEdgeVertex2;

    // True if the shapes were overlapping in the previous frame and are
    // still overlapping on the same axis in this frame
    bool isTemporalCoherenceValid = false;

    LastFrameCollisionInfo& lastFrameInfo = narrowPhaseInfo->overlappingPair->getLastFrameCollisionInfo();

    // If the shapes are not triangles (no temporal coherence for triangle collision because we do not store previous
    // frame collision data per triangle)
    if (polyhedron->getType() != CollisionShapeType::TRIANGLE) {

        // If the last frame collision info is valid and was also using SAT algorithm
        if (lastFrameInfo.isValid && lastFrameInfo.wasUsingSAT) {

            // We perform temporal coherence, we check if there is still an overlapping along the previous minimum separating
            // axis. If it is the case, we directly report the collision without executing the whole SAT algorithm again. If
            // the shapes are still separated along this axis, we directly exit with no collision.

            // If the previous minimum separation axis was a face normal of the polyhedron
            if (lastFrameInfo.satIsAxisFacePolyhedron1) {

                Vector3 outFaceNormalCapsuleSpace;

                // Compute the penetration depth along the polyhedron face normal direction
                const decimal penetrationDepth = computePolyhedronFaceVsCapsulePenetrationDepth(lastFrameInfo.satMinAxisFaceIndex, polyhedron,
                                                                                                capsuleShape, polyhedronToCapsuleTransform,
                                                                                                outFaceNormalCapsuleSpace);

                // If the previous axis is a separating axis
                if (penetrationDepth <= decimal(0.0)) {

                    // Return no collision
                    return false;
                }

                // The two shapes are overlapping as in the previous frame and on the same axis, therefore
                // we will skip the entire SAT algorithm because the minimum separating axis did not change
                isTemporalCoherenceValid = lastFrameInfo.wasColliding;

                if (isTemporalCoherenceValid) {

                    minPenetrationDepth = penetrationDepth;
                    minFaceIndex = lastFrameInfo.satMinAxisFaceIndex;
                    isMinPenetrationFaceNormal = true;
                    separatingAxisCapsuleSpace = outFaceNormalCapsuleSpace;
                }
            }
            else {   // If the previous minimum separation axis the cross product of the capsule inner segment and an edge of the polyhedron

                // Get an edge from the polyhedron (convert it into the capsule local-space)
                HalfEdgeStructure::Edge edge = polyhedron->getHalfEdge(lastFrameInfo.satMinEdge1Index);
                const Vector3 edgeVertex1 = polyhedron->getVertexPosition(edge.vertexIndex);
                const Vector3 edgeVertex2 = polyhedron->getVertexPosition(polyhedron->getHalfEdge(edge.nextEdgeIndex).vertexIndex);
                const Vector3 edgeDirectionCapsuleSpace = polyhedronToCapsuleTransform.getOrientation() * (edgeVertex2 - edgeVertex1);

                Vector3 outAxis;

                // Compute the penetration depth along this axis
                const decimal penetrationDepth = computeEdgeVsCapsuleInnerSegmentPenetrationDepth(polyhedron, capsuleShape,
                                                                                                  capsuleSegmentAxis, edgeVertex1,
                                                                                                  edgeDirectionCapsuleSpace,
                                                                                                  polyhedronToCapsuleTransform,
                                                                                                  outAxis);

                // If the previous axis is a separating axis
                if (penetrationDepth <= decimal(0.0)) {

                    // Return no collision
                    return false;
                }

                // The two shapes are overlapping as in the previous frame and on the same axis, therefore
                // we will skip the entire SAT algorithm because the minimum separating axis did not change
                isTemporalCoherenceValid = lastFrameInfo.wasColliding;

                if (isTemporalCoherenceValid) {

                    minPenetrationDepth = penetrationDepth;
                    minEdgeIndex = lastFrameInfo.satMinEdge1Index;
                    isMinPenetrationFaceNormal = false;
                    separatingAxisCapsuleSpace = outAxis;
                    separatingPolyhedronEdgeVertex1 = edgeVertex1;
                    separatingPolyhedronEdgeVertex2 = edgeVertex2;
                }
            }
        }
    }

    // If the shapes are still overlapping in the same axis as in the previous frame
    // the previous frame, we skip the whole SAT algorithm
    if (!isTemporalCoherenceValid) {

        // For each face of the convex mesh
        for (uint f = 0; f < polyhedron->getNbFaces(); f++) {

            Vector3 outFaceNormalCapsuleSpace;

            // Compute the penetration depth
            const decimal penetrationDepth = computePolyhedronFaceVsCapsulePenetrationDepth(f, polyhedron, capsuleShape,
                                                                                            polyhedronToCapsuleTransform,
                                                                                            outFaceNormalCapsuleSpace);

            // If the penetration depth is negative, we have found a separating axis
            if (penetrationDepth <= decimal(0.0)) {

                lastFrameInfo.satIsAxisFacePolyhedron1 = true;
                lastFrameInfo.satMinAxisFaceIndex = f;

                return false;
            }

            // Check if we have found a new minimum penetration axis
            if (penetrationDepth < minPenetrationDepth) {
                minPenetrationDepth = penetrationDepth;
                minFaceIndex = f;
                isMinPenetrationFaceNormal = true;
                separatingAxisCapsuleSpace = outFaceNormalCapsuleSpace;
            }
        }

        // For each direction that is the cross product of the capsule inner segment and an edge of the polyhedron
        for (uint e = 0; e < polyhedron->getNbHalfEdges(); e += 2) {

            // Get an edge from the polyhedron (convert it into the capsule local-space)
            HalfEdgeStructure::Edge edge = polyhedron->getHalfEdge(e);
            const Vector3 edgeVertex1 = polyhedron->getVertexPosition(edge.vertexIndex);
            const Vector3 edgeVertex2 = polyhedron->getVertexPosition(polyhedron->getHalfEdge(edge.nextEdgeIndex).vertexIndex);
            const Vector3 edgeDirectionCapsuleSpace = polyhedronToCapsuleTransform.getOrientation() * (edgeVertex2 - edgeVertex1);

            HalfEdgeStructure::Edge twinEdge = polyhedron->getHalfEdge(edge.twinEdgeIndex);
            const Vector3 adjacentFace1Normal = polyhedronToCapsuleTransform.getOrientation() * polyhedron->getFaceNormal(edge.faceIndex);
            const Vector3 adjacentFace2Normal = polyhedronToCapsuleTransform.getOrientation() * polyhedron->getFaceNormal(twinEdge.faceIndex);

            // Check using the Gauss Map if this edge cross product can be as separating axis
            if (isMinkowskiFaceCapsuleVsEdge(capsuleSegmentAxis, adjacentFace1Normal, adjacentFace2Normal)) {

                Vector3 outAxis;

                // Compute the penetration depth
                const decimal penetrationDepth = computeEdgeVsCapsuleInnerSegmentPenetrationDepth(polyhedron, capsuleShape,
                                                                                                  capsuleSegmentAxis, edgeVertex1,
                                                                                                  edgeDirectionCapsuleSpace,
                                                                                                  polyhedronToCapsuleTransform,
                                                                                                  outAxis);

                // If the penetration depth is negative, we have found a separating axis
                if (penetrationDepth <= decimal(0.0)) {

                    lastFrameInfo.satIsAxisFacePolyhedron1 = false;
                    lastFrameInfo.satMinEdge1Index = e;

                    return false;
                }

                // Check if we have found a new minimum penetration axis
                if (penetrationDepth < minPenetrationDepth) {
                    minPenetrationDepth = penetrationDepth;
                    minEdgeIndex = e;
                    isMinPenetrationFaceNormal = false;
                    separatingAxisCapsuleSpace = outAxis;
                    separatingPolyhedronEdgeVertex1 = edgeVertex1;
                    separatingPolyhedronEdgeVertex2 = edgeVertex2;
                }
            }
        }

    }

    // Convert the inner capsule segment points into the polyhedron local-space
    const Transform capsuleToPolyhedronTransform = polyhedronToCapsuleTransform.getInverse();
    const Vector3 capsuleSegAPolyhedronSpace = capsuleToPolyhedronTransform * capsuleSegA;
    const Vector3 capsuleSegBPolyhedronSpace = capsuleToPolyhedronTransform * capsuleSegB;

    const Vector3 normalWorld = capsuleToWorld.getOrientation() * separatingAxisCapsuleSpace;
    const decimal capsuleRadius = capsuleShape->getRadius();

    // If the separating axis is a face normal
    // We need to clip the inner capsule segment with the adjacent faces of the separating face
    if (isMinPenetrationFaceNormal) {

        computeCapsulePolyhedronFaceContactPoints(minFaceIndex, capsuleRadius, polyhedron, minPenetrationDepth,
                                                  polyhedronToCapsuleTransform, normalWorld, separatingAxisCapsuleSpace,
                                                  capsuleSegAPolyhedronSpace, capsuleSegBPolyhedronSpace,
                                                  contactManifoldInfo, isCapsuleShape1);

         lastFrameInfo.satIsAxisFacePolyhedron1 = true;
         lastFrameInfo.satMinAxisFaceIndex = minFaceIndex;
    }
    else {   // The separating axis is the cross product of a polyhedron edge and the inner capsule segment

        // Compute the closest points between the inner capsule segment and the
        // edge of the polyhedron in polyhedron local-space
        Vector3 closestPointPolyhedronEdge, closestPointCapsuleInnerSegment;
        computeClosestPointBetweenTwoSegments(capsuleSegAPolyhedronSpace, capsuleSegBPolyhedronSpace,
                                              separatingPolyhedronEdgeVertex1, separatingPolyhedronEdgeVertex2,
                                              closestPointCapsuleInnerSegment, closestPointPolyhedronEdge);


        // Project closest capsule inner segment point into the capsule bounds
        const Vector3 contactPointCapsule = (polyhedronToCapsuleTransform * closestPointCapsuleInnerSegment) - separatingAxisCapsuleSpace * capsuleRadius;

        // Create the contact point
        contactManifoldInfo.addContactPoint(normalWorld, minPenetrationDepth,
                                            isCapsuleShape1 ? contactPointCapsule : closestPointPolyhedronEdge,
                                            isCapsuleShape1 ? closestPointPolyhedronEdge : contactPointCapsule);

         lastFrameInfo.satIsAxisFacePolyhedron1 = false;
         lastFrameInfo.satMinEdge1Index = minEdgeIndex;
    }

    return true;
}

// Compute the penetration depth when the separating axis is the cross product of polyhedron edge and capsule inner segment
decimal SATAlgorithm::computeEdgeVsCapsuleInnerSegmentPenetrationDepth(const ConvexPolyhedronShape* polyhedron, const CapsuleShape* capsule,
                                                                       const Vector3& capsuleSegmentAxis, const Vector3& edgeVertex1,
                                                                       const Vector3& edgeDirectionCapsuleSpace,
                                                                       const Transform& polyhedronToCapsuleTransform, Vector3& outAxis) const {

    decimal penetrationDepth = DECIMAL_LARGEST;

    // Compute the axis to test (cross product between capsule inner segment and polyhedron edge)
    outAxis = capsuleSegmentAxis.cross(edgeDirectionCapsuleSpace);

    // Skip separating axis test if polyhedron edge is parallel to the capsule inner segment
    if (outAxis.lengthSquare() >= decimal(0.00001)) {

        const Vector3 polyhedronCentroid = polyhedronToCapsuleTransform * polyhedron->getCentroid();
        const Vector3 pointOnPolyhedronEdge = polyhedronToCapsuleTransform * edgeVertex1;

        // Swap axis direction if necessary such that it points out of the polyhedron
        if (outAxis.dot(pointOnPolyhedronEdge - polyhedronCentroid) < 0) {
            outAxis = -outAxis;
        }

        outAxis.normalize();

        // Compute the penetration depth
        const Vector3 capsuleSupportPoint = capsule->getLocalSupportPointWithMargin(-outAxis, nullptr);
        const Vector3 capsuleSupportPointToEdgePoint = pointOnPolyhedronEdge - capsuleSupportPoint;
        penetrationDepth = capsuleSupportPointToEdgePoint.dot(outAxis);
    }

    return penetrationDepth;
}

// Compute the penetration depth between the face of a polyhedron and a capsule along the polyhedron face normal direction
decimal SATAlgorithm::computePolyhedronFaceVsCapsulePenetrationDepth(uint polyhedronFaceIndex, const ConvexPolyhedronShape* polyhedron,
                                                                     const CapsuleShape* capsule, const Transform& polyhedronToCapsuleTransform,
                                                                     Vector3& outFaceNormalCapsuleSpace) const {

    // Get the face
    HalfEdgeStructure::Face face = polyhedron->getFace(polyhedronFaceIndex);

    // Get the face normal
    const Vector3 faceNormal = polyhedron->getFaceNormal(polyhedronFaceIndex);

    // Compute the penetration depth (using the capsule support in the direction opposite to the face normal)
    outFaceNormalCapsuleSpace = polyhedronToCapsuleTransform.getOrientation() * faceNormal;
    const Vector3 capsuleSupportPoint = capsule->getLocalSupportPointWithMargin(-outFaceNormalCapsuleSpace, nullptr);
    const Vector3 pointOnPolyhedronFace = polyhedronToCapsuleTransform * polyhedron->getVertexPosition(face.faceVertices[0]);
    const Vector3 capsuleSupportPointToFacePoint =  pointOnPolyhedronFace - capsuleSupportPoint;
    const decimal penetrationDepth = capsuleSupportPointToFacePoint.dot(outFaceNormalCapsuleSpace);

    return penetrationDepth;
}

// Compute the two contact points between a polyhedron and a capsule when the separating
// axis is a face normal of the polyhedron
void SATAlgorithm::computeCapsulePolyhedronFaceContactPoints(uint referenceFaceIndex, decimal capsuleRadius, const ConvexPolyhedronShape* polyhedron,
                                                             decimal penetrationDepth, const Transform& polyhedronToCapsuleTransform,
                                                             const Vector3& normalWorld, const Vector3& separatingAxisCapsuleSpace,
                                                             const Vector3& capsuleSegAPolyhedronSpace, const Vector3& capsuleSegBPolyhedronSpace,
                                                             ContactManifoldInfo& contactManifoldInfo, bool isCapsuleShape1) const {

    HalfEdgeStructure::Face face = polyhedron->getFace(referenceFaceIndex);
    uint firstEdgeIndex = face.edgeIndex;
    uint edgeIndex = firstEdgeIndex;

    std::vector<Vector3> planesPoints;
    std::vector<Vector3> planesNormals;

    // For each adjacent edge of the separating face of the polyhedron
    do {
        HalfEdgeStructure::Edge edge = polyhedron->getHalfEdge(edgeIndex);
        HalfEdgeStructure::Edge twinEdge = polyhedron->getHalfEdge(edge.twinEdgeIndex);

        // Construct a clippling plane for each adjacent edge of the separting face of the polyhedron
        planesPoints.push_back(polyhedron->getVertexPosition(edge.vertexIndex));
        planesNormals.push_back(polyhedron->getFaceNormal(twinEdge.faceIndex));

        edgeIndex = edge.nextEdgeIndex;

    } while(edgeIndex != firstEdgeIndex);

    // First we clip the inner segment of the capsule with the four planes of the adjacent faces
    std::vector<Vector3> clipSegment = clipSegmentWithPlanes(capsuleSegAPolyhedronSpace, capsuleSegBPolyhedronSpace,
                                                             planesPoints, planesNormals);

    // Project the two clipped points into the polyhedron face
    const Vector3 faceNormal = polyhedron->getFaceNormal(referenceFaceIndex);
    const Vector3 contactPoint1Polyhedron = clipSegment[0] + faceNormal * (penetrationDepth - capsuleRadius);
    const Vector3 contactPoint2Polyhedron = clipSegment[1] + faceNormal * (penetrationDepth - capsuleRadius);

    // Project the two clipped points into the capsule bounds
    const Vector3 contactPoint1Capsule = (polyhedronToCapsuleTransform * clipSegment[0]) - separatingAxisCapsuleSpace * capsuleRadius;
    const Vector3 contactPoint2Capsule = (polyhedronToCapsuleTransform * clipSegment[1]) - separatingAxisCapsuleSpace * capsuleRadius;

    // Create the contact points
    contactManifoldInfo.addContactPoint(normalWorld, penetrationDepth,
                                        isCapsuleShape1 ? contactPoint1Capsule : contactPoint1Polyhedron,
                                        isCapsuleShape1 ? contactPoint1Polyhedron : contactPoint1Capsule);
    contactManifoldInfo.addContactPoint(normalWorld, penetrationDepth,
                                        isCapsuleShape1 ? contactPoint2Capsule : contactPoint2Polyhedron,
                                        isCapsuleShape1 ? contactPoint2Polyhedron : contactPoint2Capsule);
}

// This method returns true if an edge of a polyhedron and a capsule forms a
// face of the Minkowski Difference. This test is used to know if two edges
// (one edge of the polyhedron vs the inner segment of the capsule in this case)
// have to be test as a possible separating axis
bool SATAlgorithm::isMinkowskiFaceCapsuleVsEdge(const Vector3& capsuleSegment, const Vector3& edgeAdjacentFace1Normal,
                                                const Vector3& edgeAdjacentFace2Normal) const {

    // Return true if the arc on the Gauss Map corresponding to the polyhedron edge
    // intersect the unit circle plane corresponding to capsule Gauss Map
    return capsuleSegment.dot(edgeAdjacentFace1Normal) * capsuleSegment.dot(edgeAdjacentFace2Normal) < decimal(0.0);
}

// Test collision between two convex polyhedrons
bool SATAlgorithm::testCollisionConvexPolyhedronVsConvexPolyhedron(const NarrowPhaseInfo* narrowPhaseInfo,
                                                                   ContactManifoldInfo& contactManifoldInfo) const {

    PROFILE("SATAlgorithm::testCollisionConvexPolyhedronVsConvexPolyhedron()");

    assert(narrowPhaseInfo->collisionShape1->getType() == CollisionShapeType::CONVEX_POLYHEDRON);
    assert(narrowPhaseInfo->collisionShape2->getType() == CollisionShapeType::CONVEX_POLYHEDRON);

    const ConvexPolyhedronShape* polyhedron1 = static_cast<const ConvexPolyhedronShape*>(narrowPhaseInfo->collisionShape1);
    const ConvexPolyhedronShape* polyhedron2 = static_cast<const ConvexPolyhedronShape*>(narrowPhaseInfo->collisionShape2);

    const Transform polyhedron1ToPolyhedron2 = narrowPhaseInfo->shape2ToWorldTransform.getInverse() * narrowPhaseInfo->shape1ToWorldTransform;
    const Transform polyhedron2ToPolyhedron1 = polyhedron1ToPolyhedron2.getInverse();

    decimal minPenetrationDepth = DECIMAL_LARGEST;
    uint minFaceIndex = 0;
    bool isMinPenetrationFaceNormal = false;
    bool isMinPenetrationFaceNormalPolyhedron1 = false;
    uint minSeparatingEdge1Index, minSeparatingEdge2Index;
    Vector3 separatingEdge1A, separatingEdge1B;
    Vector3 separatingEdge2A, separatingEdge2B;
    Vector3 minEdgeVsEdgeSeparatingAxisPolyhedron2Space;

    LastFrameCollisionInfo& lastFrameInfo = narrowPhaseInfo->overlappingPair->getLastFrameCollisionInfo();

    // True if the shapes were overlapping in the previous frame and are
    // still overlapping on the same axis in this frame
    bool isTemporalCoherenceValid = false;

    // If the shapes are not triangles (no temporal coherence for triangle collision because we do not store previous
    // frame collision data per triangle)
    if (polyhedron1->getType() != CollisionShapeType::TRIANGLE && polyhedron2->getType() != CollisionShapeType::TRIANGLE) {

        // If the last frame collision info is valid and was also using SAT algorithm
        if (lastFrameInfo.isValid && lastFrameInfo.wasUsingSAT) {

            // We perform temporal coherence, we check if there is still an overlapping along the previous minimum separating
            // axis. If it is the case, we directly report the collision without executing the whole SAT algorithm again. If
            // the shapes are still separated along this axis, we directly exit with no collision.

            // If the previous separating axis (or axis with minimum penetration depth)
            // was a face normal of polyhedron 1
            if (lastFrameInfo.satIsAxisFacePolyhedron1) {

                decimal penetrationDepth = testSingleFaceDirectionPolyhedronVsPolyhedron(polyhedron1, polyhedron2, polyhedron1ToPolyhedron2,
                                                                                         lastFrameInfo.satMinAxisFaceIndex);
                // If the previous axis is a separating axis
                if (penetrationDepth <= decimal(0.0)) {

                    // Return no collision
                    return false;
                }

                // The two shapes are overlapping as in the previous frame and on the same axis, therefore
                // we will skip the entire SAT algorithm because the minimum separating axis did not change
                isTemporalCoherenceValid = lastFrameInfo.wasColliding;

                if (isTemporalCoherenceValid) {

                    minPenetrationDepth = penetrationDepth;
                    minFaceIndex = lastFrameInfo.satMinAxisFaceIndex;
                    isMinPenetrationFaceNormal = true;
                    isMinPenetrationFaceNormalPolyhedron1 = true;
                }
            }
            else if (lastFrameInfo.satIsAxisFacePolyhedron2) { // If the previous separating axis (or axis with minimum penetration depth)
                                                               // was a face normal of polyhedron 2

                decimal penetrationDepth = testSingleFaceDirectionPolyhedronVsPolyhedron(polyhedron2, polyhedron1, polyhedron2ToPolyhedron1,
                                                                                         lastFrameInfo.satMinAxisFaceIndex);
                // If the previous axis is a separating axis
                if (penetrationDepth <= decimal(0.0)) {

                    // Return no collision
                    return false;
                }

                // The two shapes are overlapping as in the previous frame and on the same axis, therefore
                // we will skip the entire SAT algorithm because the minimum separating axis did not change
                isTemporalCoherenceValid = lastFrameInfo.wasColliding;

                if (isTemporalCoherenceValid) {

                    minPenetrationDepth = penetrationDepth;
                    minFaceIndex = lastFrameInfo.satMinAxisFaceIndex;
                    isMinPenetrationFaceNormal = true;
                    isMinPenetrationFaceNormalPolyhedron1 = false;
                }
            }
            else {   // If the previous separating axis (or axis with minimum penetration depth) was the cross product of two edges

                HalfEdgeStructure::Edge edge1 = polyhedron1->getHalfEdge(lastFrameInfo.satMinEdge1Index);
                HalfEdgeStructure::Edge edge2 = polyhedron2->getHalfEdge(lastFrameInfo.satMinEdge2Index);

                Vector3 separatingAxisPolyhedron2Space;

                const Vector3 edge1A = polyhedron1ToPolyhedron2 * polyhedron1->getVertexPosition(edge1.vertexIndex);
                const Vector3 edge1B = polyhedron1ToPolyhedron2 * polyhedron1->getVertexPosition(polyhedron1->getHalfEdge(edge1.nextEdgeIndex).vertexIndex);
                const Vector3 edge1Direction = edge1B - edge1A;
                const Vector3 edge2A = polyhedron2->getVertexPosition(edge2.vertexIndex);
                const Vector3 edge2B = polyhedron2->getVertexPosition(polyhedron2->getHalfEdge(edge2.nextEdgeIndex).vertexIndex);
                const Vector3 edge2Direction = edge2B - edge2A;

                // Compute the penetration depth
                decimal penetrationDepth = computeDistanceBetweenEdges(edge1A, edge2A, polyhedron2->getCentroid(),
                                                                       edge1Direction, edge2Direction, separatingAxisPolyhedron2Space);

                // If the previous axis is a separating axis
                if (penetrationDepth <= decimal(0.0)) {

                    // Return no collision
                    return false;
                }

                // The two shapes are overlapping as in the previous frame and on the same axis, therefore
                // we will skip the entire SAT algorithm because the minimum separating axis did not change
                isTemporalCoherenceValid = lastFrameInfo.wasColliding;

                if (isTemporalCoherenceValid) {

                    minPenetrationDepth = penetrationDepth;
                    isMinPenetrationFaceNormal = false;
                    isMinPenetrationFaceNormalPolyhedron1 = false;
                    minSeparatingEdge1Index = lastFrameInfo.satMinEdge1Index;
                    minSeparatingEdge2Index = lastFrameInfo.satMinEdge2Index;
                    separatingEdge1A = edge1A;
                    separatingEdge1B = edge1B;
                    separatingEdge2A = edge2A;
                    separatingEdge2B = edge2B;
                    minEdgeVsEdgeSeparatingAxisPolyhedron2Space = separatingAxisPolyhedron2Space;
                }
            }
        }
    }

    // We the shapes are still overlapping in the same axis as in
    // the previous frame, we skip the whole SAT algorithm
    if (!isTemporalCoherenceValid) {

        // Test all the face normals of the polyhedron 1 for separating axis
        uint faceIndex;
        decimal penetrationDepth = testFacesDirectionPolyhedronVsPolyhedron(polyhedron1, polyhedron2, polyhedron1ToPolyhedron2, faceIndex);
        if (penetrationDepth <= decimal(0.0)) {

            lastFrameInfo.satIsAxisFacePolyhedron1 = true;
            lastFrameInfo.satIsAxisFacePolyhedron2 = false;
            lastFrameInfo.satMinAxisFaceIndex = faceIndex;

            // We have found a separating axis
            return false;
        }
        if (penetrationDepth < minPenetrationDepth - SAME_SEPARATING_AXIS_BIAS) {
            isMinPenetrationFaceNormal = true;
            minPenetrationDepth = penetrationDepth;
            minFaceIndex = faceIndex;
            isMinPenetrationFaceNormalPolyhedron1 = true;
        }

        // Test all the face normals of the polyhedron 2 for separating axis
        penetrationDepth = testFacesDirectionPolyhedronVsPolyhedron(polyhedron2, polyhedron1, polyhedron2ToPolyhedron1, faceIndex);
        if (penetrationDepth <= decimal(0.0)) {

            lastFrameInfo.satIsAxisFacePolyhedron1 = false;
            lastFrameInfo.satIsAxisFacePolyhedron2 = true;
            lastFrameInfo.satMinAxisFaceIndex = faceIndex;

            // We have found a separating axis
            return false;
        }
        if (penetrationDepth < minPenetrationDepth - SAME_SEPARATING_AXIS_BIAS) {
            isMinPenetrationFaceNormal = true;
            minPenetrationDepth = penetrationDepth;
            minFaceIndex = faceIndex;
            isMinPenetrationFaceNormalPolyhedron1 = false;
        }

        // Test the cross products of edges of polyhedron 1 with edges of polyhedron 2 for separating axis
        for (uint i=0; i < polyhedron1->getNbHalfEdges(); i += 2) {

            // Get an edge of polyhedron 1
            HalfEdgeStructure::Edge edge1 = polyhedron1->getHalfEdge(i);

            const Vector3 edge1A = polyhedron1ToPolyhedron2 * polyhedron1->getVertexPosition(edge1.vertexIndex);
            const Vector3 edge1B = polyhedron1ToPolyhedron2 * polyhedron1->getVertexPosition(polyhedron1->getHalfEdge(edge1.nextEdgeIndex).vertexIndex);
            const Vector3 edge1Direction = edge1B - edge1A;

            for (uint j=0; j < polyhedron2->getNbHalfEdges(); j += 2) {

                // Get an edge of polyhedron 2
                HalfEdgeStructure::Edge edge2 = polyhedron2->getHalfEdge(j);

                const Vector3 edge2A = polyhedron2->getVertexPosition(edge2.vertexIndex);
                const Vector3 edge2B = polyhedron2->getVertexPosition(polyhedron2->getHalfEdge(edge2.nextEdgeIndex).vertexIndex);
                const Vector3 edge2Direction = edge2B - edge2A;

                // If the two edges build a minkowski face (and the cross product is
                // therefore a candidate for separating axis
                if (testEdgesBuildMinkowskiFace(polyhedron1, edge1, polyhedron2, edge2, polyhedron1ToPolyhedron2)) {

                    Vector3 separatingAxisPolyhedron2Space;

                    // Compute the penetration depth
                    decimal penetrationDepth = computeDistanceBetweenEdges(edge1A, edge2A, polyhedron2->getCentroid(),
                                                                           edge1Direction, edge2Direction, separatingAxisPolyhedron2Space);

                    if (penetrationDepth <= decimal(0.0)) {

                        lastFrameInfo.satIsAxisFacePolyhedron1 = false;
                        lastFrameInfo.satIsAxisFacePolyhedron2 = false;
                        lastFrameInfo.satMinEdge1Index = i;
                        lastFrameInfo.satMinEdge2Index = j;

                        // We have found a separating axis
                        return false;
                    }

                    if (penetrationDepth < minPenetrationDepth - SAME_SEPARATING_AXIS_BIAS) {

                        minPenetrationDepth = penetrationDepth;
                        isMinPenetrationFaceNormalPolyhedron1 = false;
                        isMinPenetrationFaceNormal = false;
                        minSeparatingEdge1Index = i;
                        minSeparatingEdge2Index = j;
                        separatingEdge1A = edge1A;
                        separatingEdge1B = edge1B;
                        separatingEdge2A = edge2A;
                        separatingEdge2B = edge2B;
                        minEdgeVsEdgeSeparatingAxisPolyhedron2Space = separatingAxisPolyhedron2Space;
                    }

                }
            }
        }
    }

    // Here we know the shapes are overlapping on a given minimum separating axis.
    // Now, we will clip the shapes along this axis to find the contact points

    assert(minPenetrationDepth > decimal(0.0));
    assert((isMinPenetrationFaceNormal && minFaceIndex >= 0) || !isMinPenetrationFaceNormal);

    // If the minimum separating axis is a face normal
    if (isMinPenetrationFaceNormal) {

        const ConvexPolyhedronShape* referencePolyhedron = isMinPenetrationFaceNormalPolyhedron1 ? polyhedron1 : polyhedron2;
        const ConvexPolyhedronShape* incidentPolyhedron = isMinPenetrationFaceNormalPolyhedron1 ? polyhedron2 : polyhedron1;
        const Transform& referenceToIncidentTransform = isMinPenetrationFaceNormalPolyhedron1 ? polyhedron1ToPolyhedron2 : polyhedron2ToPolyhedron1;
        const Transform& incidentToReferenceTransform = isMinPenetrationFaceNormalPolyhedron1 ? polyhedron2ToPolyhedron1 : polyhedron1ToPolyhedron2;

        assert(minPenetrationDepth > decimal(0.0));

        const Vector3 axisReferenceSpace = referencePolyhedron->getFaceNormal(minFaceIndex);
        const Vector3 axisIncidentSpace = referenceToIncidentTransform.getOrientation() * axisReferenceSpace;

        // Compute the world normal
        const Vector3 normalWorld = isMinPenetrationFaceNormalPolyhedron1 ? narrowPhaseInfo->shape1ToWorldTransform.getOrientation() * axisReferenceSpace :
                                                                            -(narrowPhaseInfo->shape2ToWorldTransform.getOrientation() * axisReferenceSpace);

        // Get the reference face
        HalfEdgeStructure::Face referenceFace = referencePolyhedron->getFace(minFaceIndex);

        // Find the incident face on the other polyhedron (most anti-parallel face)
        uint incidentFaceIndex = findMostAntiParallelFaceOnPolyhedron(incidentPolyhedron, axisIncidentSpace);

        // Get the incident face
        HalfEdgeStructure::Face incidentFace = incidentPolyhedron->getFace(incidentFaceIndex);

        std::vector<Vector3> polygonVertices;   // Vertices to clip of the incident face
        std::vector<Vector3> planesNormals;     // Normals of the clipping planes
        std::vector<Vector3> planesPoints;      // Points on the clipping planes

        // Get all the vertices of the incident face (in the reference local-space)
        std::vector<uint>::const_iterator it;
        for (it = incidentFace.faceVertices.begin(); it != incidentFace.faceVertices.end(); ++it) {
            const Vector3 faceVertexIncidentSpace = incidentPolyhedron->getVertexPosition(*it);
            polygonVertices.push_back(incidentToReferenceTransform * faceVertexIncidentSpace);
        }

        // Get the reference face clipping planes
        uint currentEdgeIndex = referenceFace.edgeIndex;
        uint firstEdgeIndex = currentEdgeIndex;
        do {

            // Get the adjacent edge
            HalfEdgeStructure::Edge edge = referencePolyhedron->getHalfEdge(currentEdgeIndex);

            // Get the twin edge
            HalfEdgeStructure::Edge twinEdge = referencePolyhedron->getHalfEdge(edge.twinEdgeIndex);

            // Get the adjacent face normal (and negate it to have a clipping plane)
            Vector3 faceNormal = -referencePolyhedron->getFaceNormal(twinEdge.faceIndex);

            // Get a vertex of the clipping plane (vertex of the adjacent edge)
            Vector3 faceVertex = referencePolyhedron->getVertexPosition(edge.vertexIndex);

            planesNormals.push_back(faceNormal);
            planesPoints.push_back(faceVertex);

            // Go to the next adjacent edge of the reference face
            currentEdgeIndex = edge.nextEdgeIndex;

        } while (currentEdgeIndex != firstEdgeIndex);

        assert(planesNormals.size() > 0);
        assert(planesNormals.size() == planesPoints.size());

        // Clip the reference faces with the adjacent planes of the reference face
        std::vector<Vector3> clipPolygonVertices = clipPolygonWithPlanes(polygonVertices, planesPoints, planesNormals);
        assert(clipPolygonVertices.size() > 0);

        // We only keep the clipped points that are below the reference face
        const Vector3 referenceFaceVertex = referencePolyhedron->getVertexPosition(firstEdgeIndex);
        std::vector<Vector3>::const_iterator itPoints;
        for (itPoints = clipPolygonVertices.begin(); itPoints != clipPolygonVertices.end(); ++itPoints) {

            // If the clip point is bellow the reference face
            if (((*itPoints) - referenceFaceVertex).dot(axisReferenceSpace) < decimal(0.0)) {

                // Convert the clip incident polyhedron vertex into the incident polyhedron local-space
                const Vector3 contactPointIncidentPolyhedron = referenceToIncidentTransform * (*itPoints);

                // Project the contact point onto the reference face
                Vector3 contactPointReferencePolyhedron = (*itPoints) + axisReferenceSpace * minPenetrationDepth;

                // Create a new contact point
                contactManifoldInfo.addContactPoint(normalWorld, minPenetrationDepth,
                                                    isMinPenetrationFaceNormalPolyhedron1 ? contactPointReferencePolyhedron : contactPointIncidentPolyhedron,
                                                    isMinPenetrationFaceNormalPolyhedron1 ? contactPointIncidentPolyhedron : contactPointReferencePolyhedron);
            }
        }

        lastFrameInfo.satIsAxisFacePolyhedron1 = isMinPenetrationFaceNormalPolyhedron1;
        lastFrameInfo.satIsAxisFacePolyhedron2 = !isMinPenetrationFaceNormalPolyhedron1;
        lastFrameInfo.satMinAxisFaceIndex = minFaceIndex;
    }
    else {    // If we have an edge vs edge contact

        // Compute the closest points between the two edges (in the local-space of poylhedron 2)
        Vector3 closestPointPolyhedron1Edge, closestPointPolyhedron2Edge;
        computeClosestPointBetweenTwoSegments(separatingEdge1A, separatingEdge1B, separatingEdge2A, separatingEdge2B,
                                              closestPointPolyhedron1Edge, closestPointPolyhedron2Edge);

        // Compute the contact point on polyhedron 1 edge in the local-space of polyhedron 1
        const Vector3 closestPointPolyhedron1EdgeLocalSpace = polyhedron2ToPolyhedron1 * closestPointPolyhedron1Edge;

        // Compute the world normal
        const Vector3 normalWorld = narrowPhaseInfo->shape2ToWorldTransform.getOrientation() * minEdgeVsEdgeSeparatingAxisPolyhedron2Space;

        // Create the contact point
        contactManifoldInfo.addContactPoint(normalWorld, minPenetrationDepth,
                                            closestPointPolyhedron1EdgeLocalSpace, closestPointPolyhedron2Edge);

        lastFrameInfo.satIsAxisFacePolyhedron1 = false;
        lastFrameInfo.satIsAxisFacePolyhedron2 = false;
        lastFrameInfo.satMinEdge1Index = minSeparatingEdge1Index;
        lastFrameInfo.satMinEdge2Index = minSeparatingEdge2Index;
    }

    return true;
}

// Find and return the index of the polyhedron face with the most anti-parallel face normal given a direction vector
// This is used to find the incident face on a polyhedron of a given reference face of another polyhedron
uint SATAlgorithm::findMostAntiParallelFaceOnPolyhedron(const ConvexPolyhedronShape* polyhedron, const Vector3& direction) const {

    decimal minDotProduct = DECIMAL_LARGEST;
    uint mostAntiParallelFace = 0;

    // For each face of the polyhedron
    for (uint i=0; i < polyhedron->getNbFaces(); i++) {

        // Get the face normal
        decimal dotProduct = polyhedron->getFaceNormal(i).dot(direction);
        if (dotProduct < minDotProduct) {
            minDotProduct = dotProduct;
            mostAntiParallelFace = i;
        }
    }

    return mostAntiParallelFace;
}

// Compute and return the distance between the two edges in the direction of the candidate separating axis
decimal SATAlgorithm::computeDistanceBetweenEdges(const Vector3& edge1A, const Vector3& edge2A, const Vector3& polyhedron2Centroid,
                                                  const Vector3& edge1Direction, const Vector3& edge2Direction,
                                                  Vector3& outSeparatingAxisPolyhedron2Space) const {

    // If the two edges are parallel
    if (areParallelVectors(edge1Direction, edge2Direction)) {

        // Return a large penetration depth to skip those edges
        return DECIMAL_LARGEST;
    }

    // Compute the candidate separating axis (cross product between two polyhedrons edges)
    Vector3 axis = edge1Direction.cross(edge2Direction).getUnit();

    // Make sure the axis direction is going from first to second polyhedron
    if (axis.dot(edge2A - polyhedron2Centroid) > decimal(0.0)) {
        axis = -axis;
    }

    outSeparatingAxisPolyhedron2Space = axis;

    // Compute and return the distance between the edges
    return -axis.dot(edge2A - edge1A);
}

// Return the penetration depth between two polyhedra along a face normal axis of the first polyhedron
decimal SATAlgorithm::testSingleFaceDirectionPolyhedronVsPolyhedron(const ConvexPolyhedronShape* polyhedron1,
                                                                    const ConvexPolyhedronShape* polyhedron2,
                                                                    const Transform& polyhedron1ToPolyhedron2,
                                                                    uint faceIndex) const {

    HalfEdgeStructure::Face face = polyhedron1->getFace(faceIndex);

    // Get the face normal
    const Vector3 faceNormal = polyhedron1->getFaceNormal(faceIndex);

    // Convert the face normal into the local-space of polyhedron 2
    const Vector3 faceNormalPolyhedron2Space = polyhedron1ToPolyhedron2.getOrientation() * faceNormal;

    // Get the support point of polyhedron 2 in the inverse direction of face normal
    const Vector3 supportPoint = polyhedron2->getLocalSupportPointWithoutMargin(-faceNormalPolyhedron2Space, nullptr);

    // Compute the penetration depth
    const Vector3 faceVertex = polyhedron1ToPolyhedron2 * polyhedron1->getVertexPosition(face.faceVertices[0]);
    decimal penetrationDepth = (faceVertex - supportPoint).dot(faceNormalPolyhedron2Space);

    return penetrationDepth;
}

// Test all the normals of a polyhedron for separating axis in the polyhedron vs polyhedron case
decimal SATAlgorithm::testFacesDirectionPolyhedronVsPolyhedron(const ConvexPolyhedronShape* polyhedron1,
                                                               const ConvexPolyhedronShape* polyhedron2,
                                                               const Transform& polyhedron1ToPolyhedron2,
                                                               uint& minFaceIndex) const {

    decimal minPenetrationDepth = DECIMAL_LARGEST;

    // For each face of the first polyhedron
    for (uint f = 0; f < polyhedron1->getNbFaces(); f++) {

        decimal penetrationDepth = testSingleFaceDirectionPolyhedronVsPolyhedron(polyhedron1, polyhedron2,
                                                                                 polyhedron1ToPolyhedron2, f);

        // If the penetration depth is negative, we have found a separating axis
        if (penetrationDepth <= decimal(0.0)) {
            minFaceIndex = f;
            return penetrationDepth;
        }

        // Check if we have found a new minimum penetration axis
        if (penetrationDepth < minPenetrationDepth) {
            minPenetrationDepth = penetrationDepth;
            minFaceIndex = f;
        }
    }

    return minPenetrationDepth;
}


// Return true if two edges of two polyhedrons build a minkowski face (and can therefore be a separating axis)
bool SATAlgorithm::testEdgesBuildMinkowskiFace(const ConvexPolyhedronShape* polyhedron1, const HalfEdgeStructure::Edge& edge1,
                                               const ConvexPolyhedronShape* polyhedron2, const HalfEdgeStructure::Edge& edge2,
                                               const Transform& polyhedron1ToPolyhedron2) const {

    const Vector3 a = polyhedron1ToPolyhedron2 * polyhedron1->getFaceNormal(edge1.faceIndex);
    const Vector3 b = polyhedron1ToPolyhedron2 * polyhedron1->getFaceNormal(polyhedron1->getHalfEdge(edge1.twinEdgeIndex).faceIndex);

    const Vector3 c = polyhedron2->getFaceNormal(edge2.faceIndex);
    const Vector3 d = polyhedron2->getFaceNormal(polyhedron2->getHalfEdge(edge2.twinEdgeIndex).faceIndex);

    // Compute b.cross(a) using the edge direction
    const Vector3 edge1Vertex1 = polyhedron1->getVertexPosition(edge1.vertexIndex);
    const Vector3 edge1Vertex2 = polyhedron1->getVertexPosition(polyhedron1->getHalfEdge(edge1.twinEdgeIndex).vertexIndex);
    const Vector3 bCrossA = polyhedron1ToPolyhedron2.getOrientation() * (edge1Vertex2 - edge1Vertex1);

    // Compute d.cross(c) using the edge direction
    const Vector3 edge2Vertex1 = polyhedron2->getVertexPosition(edge2.vertexIndex);
    const Vector3 edge2Vertex2 = polyhedron2->getVertexPosition(polyhedron2->getHalfEdge(edge2.twinEdgeIndex).vertexIndex);
    const Vector3 dCrossC = edge2Vertex2 - edge2Vertex1;

    // Test if the two arcs of the Gauss Map intersect (therefore forming a minkowski face)
    // Note that we negate the normals of the second polyhedron because we are looking at the
    // Gauss map of the minkowski difference of the polyhedrons
    return testGaussMapArcsIntersect(a, b, -c, -d, bCrossA, dCrossC);
}


// Return true if the arcs AB and CD on the Gauss Map (unit sphere) intersect
/// This is used to know if the edge between faces with normal A and B on first polyhedron
/// and edge between faces with normal C and D on second polygon create a face on the Minkowski
/// sum of both polygons. If this is the case, it means that the cross product of both edges
/// might be a separating axis.
bool SATAlgorithm::testGaussMapArcsIntersect(const Vector3& a, const Vector3& b, const Vector3& c, const Vector3& d,
                                             const Vector3& bCrossA, const Vector3& dCrossC) const {

    const decimal cba = c.dot(bCrossA);
    const decimal dba = d.dot(bCrossA);
    const decimal adc = a.dot(dCrossC);
    const decimal bdc = b.dot(dCrossC);

    return cba * dba < decimal(0.0) && adc * bdc < decimal(0.0) && cba * bdc > decimal(0.0);
}
