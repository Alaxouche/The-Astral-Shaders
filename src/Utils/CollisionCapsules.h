#pragma once

#include "Utils/ActorUtils.h"

#include <span>

// World-space capsules for collision shapes, from Open Shaders' ActorUtils (fur touch and body capsules).
namespace Util
{
	/** @brief World-space capsule used to approximate an actor collision shape. */
	struct ShapeCollisionCapsule
	{
		RE::NiPoint3 pointA;
		RE::NiPoint3 pointB;
		float radius;
	};

	/**
	 * @brief Extracts a world-space capsule from a collision object. Native capsules keep their endpoints and radius;
	 * other supported shapes return a degenerate capsule with coincident endpoints.
	 * @return True when a supported shape was extracted.
	 */
	bool GetShapeCollisionCapsule(RE::bhkNiCollisionObject* collisionObj, ShapeCollisionCapsule& capsule);

	/**
	 * @brief Reduces a rigid body's shape to world-space capsules, one per part of a shape list.
	 * @return How many entries of a_capsules were filled.
	 */
	size_t GetRigidBodyCapsules(RE::bhkRigidBody* a_body, std::span<ShapeCollisionCapsule> a_capsules);
}
