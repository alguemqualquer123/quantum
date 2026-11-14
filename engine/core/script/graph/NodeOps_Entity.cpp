// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "NodeOps.h"

#include "Engine.h"
#include "action/Particles.h"
#include "ecs/components/TagComponent.h"
#include "manager/BundleManager.h"
#include "math/Quaternion.h"
#include "component/LightComponent.h"
#include "component/ParticlesComponent.h"
#include "component/TextComponent.h"
#include "object/Light.h"

#include <cctype>
#include <cmath>
#include <cstring>

namespace quantum {
namespace graph {

    namespace {

        // entity.hasComponent cannot ask a registry for a type by name: the engine keeps
        // typeid hashes only. This table is the hand-written mirror of exactly what
        // EntityRegistry's constructor registers, keyed by the short name the node shows
        // ("Mesh"), with the "Component" suffix accepted too.
        typedef bool (*ComponentProbe)(EntityRegistry&, Entity);

        template <typename T>
        bool probeComponent(EntityRegistry& registry, Entity entity) {
            return registry.findComponent<T>(entity) != nullptr;
        }

        struct ComponentRow {
            const char* name;
            ComponentProbe probe;
        };

        const ComponentRow kComponentRows[] = {
            {"Mesh", &probeComponent<MeshComponent>},
            {"Model", &probeComponent<ModelComponent>},
            {"Bone", &probeComponent<BoneComponent>},
            {"Sky", &probeComponent<SkyComponent>},
            {"Fog", &probeComponent<FogComponent>},
            {"Mirror", &probeComponent<MirrorComponent>},
            {"UIContainer", &probeComponent<UIContainerComponent>},
            {"UILayout", &probeComponent<UILayoutComponent>},
            {"Sprite", &probeComponent<SpriteComponent>},
            {"SpriteAnimation", &probeComponent<SpriteAnimationComponent>},
            {"Transform", &probeComponent<Transform>},
            {"Camera", &probeComponent<CameraComponent>},
            {"Light", &probeComponent<LightComponent>},
            {"Light2D", &probeComponent<Light2DComponent>},
            {"Occluder2D", &probeComponent<Occluder2DComponent>},
            {"Action", &probeComponent<ActionComponent>},
            {"TimedAction", &probeComponent<TimedActionComponent>},
            {"PositionAction", &probeComponent<PositionActionComponent>},
            {"RotationAction", &probeComponent<RotationActionComponent>},
            {"ScaleAction", &probeComponent<ScaleActionComponent>},
            {"ColorAction", &probeComponent<ColorActionComponent>},
            {"AlphaAction", &probeComponent<AlphaActionComponent>},
            {"Particles", &probeComponent<ParticlesComponent>},
            {"Points", &probeComponent<PointsComponent>},
            {"Lines", &probeComponent<LinesComponent>},
            {"Text", &probeComponent<TextComponent>},
            {"UI", &probeComponent<UIComponent>},
            {"Image", &probeComponent<ImageComponent>},
            {"Button", &probeComponent<ButtonComponent>},
            {"Panel", &probeComponent<PanelComponent>},
            {"Scrollbar", &probeComponent<ScrollbarComponent>},
            {"Progressbar", &probeComponent<ProgressbarComponent>},
            {"TextEdit", &probeComponent<TextEditComponent>},
            {"MeshPolygon", &probeComponent<MeshPolygonComponent>},
            {"Polygon", &probeComponent<PolygonComponent>},
            {"Animation", &probeComponent<AnimationComponent>},
            {"KeyframeTracks", &probeComponent<KeyframeTracksComponent>},
            {"MorphTracks", &probeComponent<MorphTracksComponent>},
            {"RotateTracks", &probeComponent<RotateTracksComponent>},
            {"TranslateTracks", &probeComponent<TranslateTracksComponent>},
            {"ScaleTracks", &probeComponent<ScaleTracksComponent>},
            {"Script", &probeComponent<ScriptComponent>},
            {"Terrain", &probeComponent<TerrainComponent>},
            {"Sound", &probeComponent<SoundComponent>},
            {"Tilemap", &probeComponent<TilemapComponent>},
#ifdef QUANTUM_PHYSICS_2D
            {"Body2D", &probeComponent<Body2DComponent>},
            {"Joint2D", &probeComponent<Joint2DComponent>},
#endif
#ifdef QUANTUM_PHYSICS_3D
            {"Body3D", &probeComponent<Body3DComponent>},
            {"Joint3D", &probeComponent<Joint3DComponent>},
#endif
            {"InstancedMesh", &probeComponent<InstancedMeshComponent>},
            {"Bundle", &probeComponent<BundleComponent>},
            {"ReflectionProbe", &probeComponent<ReflectionProbeComponent>},
            {"Water", &probeComponent<WaterComponent>},
            {"WaterExclusion", &probeComponent<WaterExclusionComponent>},
            {"Tag", &probeComponent<TagComponent>}
        };

        bool sameName(const char* a, const std::string& b) {
            const size_t length = std::strlen(a);
            if (length != b.size()) {
                return false;
            }
            for (size_t i = 0; i < length; ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        }

        std::string shortenComponentName(std::string name) {
            const size_t suffix = name.rfind("Component");
            if (suffix != std::string::npos && suffix + 9 == name.size() && suffix > 0) {
                name.erase(suffix);
            }
            return name;
        }

        // A raw entity pin: unlike op::target, an unconnected pin stays NULL_ENTITY,
        // which is what a parent pin means ("no parent") rather than "myself".
        Entity pinEntity(GraphRuntime& runtime, const BoundNode& node, size_t pin) {
            const GraphValue value = runtime.readInput(node, pin);
            if (value.isNone()) {
                return NULL_ENTITY;
            }
            return value.type == GraphValueType::Entity
                ? value.asEntity()
                : static_cast<Entity>(value.asInt());
        }

        RotationOrder rotationOrder(int index) {
            const int clamped = index < 0 ? 0 : (index > 5 ? 5 : index);
            return static_cast<RotationOrder>(clamped);
        }

        void setPosition(Transform& transform, const Vector3& position) {
            if (transform.position != position) {
                transform.position = position;
                transform.needUpdate = true;
            }
        }

        void setRotation(Transform& transform, const Quaternion& rotation) {
            if (transform.rotation != rotation) {
                transform.rotation = rotation;
                transform.needUpdate = true;
            }
        }

        void setScale(Transform& transform, const Vector3& scale) {
            if (transform.scale != scale) {
                transform.scale = scale;
                transform.needUpdate = true;
            }
        }

        // ---- Transform

        void entitySelf(GraphRuntime& runtime, const BoundNode& node) {
            op::outEntity(runtime, node, 0, op::selfEntity(runtime));
        }

        void entityGetPosition(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 0));
            op::outVector3(runtime, node, 0, transform ? transform->position : Vector3());
        }

        int entitySetPosition(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 1));
            if (transform) {
                setPosition(*transform, op::vector3(runtime, node, 0));
            }
            return 0;
        }

        int entityTranslate(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 1));
            if (transform) {
                setPosition(*transform, transform->position + op::vector3(runtime, node, 0));
            }
            return 0;
        }

        int entityMove(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 2));
            if (!transform) {
                return 0;
            }
            const Vector3 direction = op::vector3(runtime, node, 0);
            const Vector3 step = direction * (static_cast<float>(op::number(runtime, node, 1) *
                                                                 Engine::getDeltatime()));
            setPosition(*transform, transform->position + step);
            return 0;
        }

        void entityGetRotation(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 0));
            op::outQuaternion(runtime, node, 0, transform ? transform->rotation : Quaternion());
        }

        int entitySetRotation(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 1));
            if (transform) {
                setRotation(*transform, op::quaternion(runtime, node, 0));
            }
            return 0;
        }

        int entitySetEulerRotation(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 2));
            if (!transform) {
                return 0;
            }
            const Vector3 euler = op::vector3(runtime, node, 0);
            Quaternion rotation;
            rotation.fromEulerAngles(euler.x, euler.y, euler.z,
                                     rotationOrder(static_cast<int>(op::integer(runtime, node, 1))));
            setRotation(*transform, rotation);
            return 0;
        }

        // Accumulated in euler space so the value a graph adds lines up with what
        // entity.setEulerRotation and the inspector show; a quaternion product would make
        // "add 90 degrees of yaw" land differently every time it ran.
        int entityRotate(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 1));
            if (!transform) {
                return 0;
            }
            const Vector3 current = transform->rotation.getEulerAngles(RotationOrder::XYZ);
            const Vector3 delta = op::vector3(runtime, node, 0);
            Quaternion rotation;
            rotation.fromEulerAngles(current.x + delta.x, current.y + delta.y, current.z + delta.z,
                                     RotationOrder::XYZ);
            setRotation(*transform, rotation);
            return 0;
        }

        int entityLookAt(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 2));
            if (!transform) {
                return 0;
            }
            const Vector3 direction = op::vector3(runtime, node, 0) - transform->worldPosition;
            if (direction.length() <= 0.0001f) {
                return 0;
            }
            setRotation(*transform, Quaternion::lookRotation(direction, op::vector3(runtime, node, 1)));
            return 0;
        }

        void entityGetScale(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 0));
            op::outVector3(runtime, node, 0, transform ? transform->scale : Vector3(1, 1, 1));
        }

        int entitySetScale(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 1));
            if (transform) {
                setScale(*transform, op::vector3(runtime, node, 0));
            }
            return 0;
        }

        int entityScaleBy(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 1));
            if (transform) {
                setScale(*transform, transform->scale * op::vector3(runtime, node, 0));
            }
            return 0;
        }

        void entityGetParent(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 0));
            op::outEntity(runtime, node, 0, transform ? transform->parent : NULL_ENTITY);
        }

        int entitySetParent(GraphRuntime& runtime, const BoundNode& node) {
            const Entity parent = pinEntity(runtime, node, 0);
            const Entity child = op::target(runtime, node, 2);
            Scene* world = op::scene(runtime);
            if (!world || !world->isEntityCreated(child)) {
                op::warn(runtime, node, "target entity does not exist");
                return 0;
            }
            if (parent == child) {
                op::warn(runtime, node, "an entity cannot parent itself");
                return 0;
            }
            if (parent != NULL_ENTITY) {
                if (!world->isEntityCreated(parent)) {
                    op::warn(runtime, node, "parent entity does not exist");
                    return 0;
                }
                // Reparenting into your own branch would make the hierarchy cyclic and the
                // transform pass would walk it forever.
                if (world->isParentOf(child, parent)) {
                    op::warn(runtime, node, "parent is a descendant of the entity");
                    return 0;
                }
            }
            world->addEntityChild(parent, child, op::flag(runtime, node, 1));
            return 0;
        }

        int entitySetVisible(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 1));
            if (!transform) {
                return 0;
            }
            const bool visible = op::flag(runtime, node, 0);
            if (transform->visible != visible) {
                transform->visible = visible;
                transform->needUpdateChildVisibility = true;
            }
            return 0;
        }

        void entityIsVisible(GraphRuntime& runtime, const BoundNode& node) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, 0));
            op::outBool(runtime, node, 0, transform && transform->visible);
        }

        // ---- Identity and lookup

        void entityGetName(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 0);
            op::outString(runtime, node, 0,
                          op::alive(runtime, entity) ? op::scene(runtime)->getEntityName(entity) : std::string());
        }

        int entitySetName(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 1);
            if (op::alive(runtime, entity)) {
                op::scene(runtime)->setEntityName(entity, op::text(runtime, node, 0));
            } else {
                op::warn(runtime, node, "target entity does not exist");
            }
            return 0;
        }

        void entityFindByName(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            op::outEntity(runtime, node, 0,
                          world ? world->findEntity(op::text(runtime, node, 0)) : NULL_ENTITY);
        }

        void entityExists(GraphRuntime& runtime, const BoundNode& node) {
            const GraphValue value = runtime.readInput(node, 0);
            const Entity entity = value.isNone()
                ? op::selfEntity(runtime)
                : (value.type == GraphValueType::Entity ? value.asEntity() : static_cast<Entity>(value.asInt()));
            op::outBool(runtime, node, 0, op::alive(runtime, entity));
        }

        void entityCount(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            op::outInt(runtime, node, 0, world ? static_cast<int64_t>(world->getEntityCount()) : 0);
        }

        int entityDestroy(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity entity = op::target(runtime, node, 0);
            if (!world || !world->isEntityCreated(entity)) {
                op::warn(runtime, node, "target entity does not exist");
                return -1;
            }

            GraphInstance& inst = op::instance(runtime);
            const bool self = entity == inst.entity;
            world->destroyEntity(entity);
            if (self) {
                runtime.retire();
                return -1;
            }
            return 0;
        }

        int entitySpawn(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            if (!world) {
                op::warn(runtime, node, "no scene to spawn into");
                return -1;
            }

            const std::string bundle = op::text(runtime, node, 0);
            const Entity root = BundleManager::createBundle(bundle, world, pinEntity(runtime, node, 3));
            op::outEntity(runtime, node, 0, root);
            if (root == NULL_ENTITY) {
                op::warn(runtime, node, "bundle '" + bundle + "' could not be instantiated");
                return 1;
            }

            // The bundle's own factory decides where its entities start, so the pins only
            // apply when the author wired them.
            Transform* transform = world->findComponent<Transform>(root);
            if (transform) {
                if (!runtime.readInput(node, 1).isNone()) {
                    setPosition(*transform, op::vector3(runtime, node, 1));
                }
                if (!runtime.readInput(node, 2).isNone()) {
                    setRotation(*transform, op::quaternion(runtime, node, 2));
                }
            }
            return 0;
        }

        Vector3 entityWorldPosition(GraphRuntime& runtime, const BoundNode& node, size_t pin) {
            Transform* transform = op::transform(runtime, node, op::target(runtime, node, pin));
            return transform ? transform->worldPosition : Vector3();
        }

        void entityDistanceTo(GraphRuntime& runtime, const BoundNode& node) {
            const Vector3 a = entityWorldPosition(runtime, node, 0);
            const Vector3 b = entityWorldPosition(runtime, node, 1);
            op::outFloat(runtime, node, 0, static_cast<double>((b - a).length()));
        }

        void entityDirectionTo(GraphRuntime& runtime, const BoundNode& node) {
            const Vector3 a = entityWorldPosition(runtime, node, 0);
            const Vector3 b = entityWorldPosition(runtime, node, 1);
            const Vector3 delta = b - a;
            op::outVector3(runtime, node, 0, delta.length() > 0.0001f ? delta.normalized() : Vector3(0, 0, 0));
        }

        void entityHasComponent(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 1);
            const std::string name = shortenComponentName(op::text(runtime, node, 0));
            Scene* world = op::scene(runtime);
            if (!world || !op::alive(runtime, entity) || name.empty()) {
                op::outBool(runtime, node, 0, false);
                return;
            }

            for (size_t i = 0; i < sizeof(kComponentRows) / sizeof(kComponentRows[0]); ++i) {
                if (sameName(kComponentRows[i].name, name)) {
                    op::outBool(runtime, node, 0, kComponentRows[i].probe(*world, entity));
                    return;
                }
            }
            op::outBool(runtime, node, 0, false);
        }

        // ---- Tags

        int entitySetTag(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 1);
            Scene* world = op::scene(runtime);
            if (!world || !op::alive(runtime, entity)) {
                op::warn(runtime, node, "target entity does not exist");
                return 0;
            }
            TagComponent* tag = world->findComponent<TagComponent>(entity);
            if (!tag) {
                world->addComponent<TagComponent>(entity);
                tag = world->findComponent<TagComponent>(entity);
            }
            if (tag) {
                tag->setTag(op::text(runtime, node, 0));
            }
            return 0;
        }

        void entityGetTag(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 0);
            TagComponent* tag = op::alive(runtime, entity)
                ? op::scene(runtime)->findComponent<TagComponent>(entity)
                : nullptr;
            op::outString(runtime, node, 0, tag ? tag->getTag() : std::string());
        }

        void entityHasTag(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 1);
            TagComponent* tag = op::alive(runtime, entity)
                ? op::scene(runtime)->findComponent<TagComponent>(entity)
                : nullptr;
            const std::string wanted = op::text(runtime, node, 0);
            op::outBool(runtime, node, 0, tag && (wanted.empty() ? tag->hasTag() : tag->isTag(wanted)));
        }

        // Walks the TagComponent array instead of the entity list: the dense array is one
        // lookup per tagged entity and never copies ids.
        void entityFindByTag(GraphRuntime& runtime, const BoundNode& node) {
            const std::string wanted = op::text(runtime, node, 0);
            Scene* world = op::scene(runtime);
            ComponentArray<TagComponent>* array = world ? world->getComponentArray<TagComponent>() : nullptr;
            Entity found = NULL_ENTITY;
            if (array) {
                for (size_t i = 0; i < array->size(); ++i) {
                    const TagComponent& tag = array->getComponentFromIndex(i);
                    if (wanted.empty() ? tag.hasTag() : tag.isTag(wanted)) {
                        found = array->getEntity(i);
                        break;
                    }
                }
            }
            op::outEntity(runtime, node, 0, found);
        }

        void entityCountTag(GraphRuntime& runtime, const BoundNode& node) {
            const std::string wanted = op::text(runtime, node, 0);
            Scene* world = op::scene(runtime);
            ComponentArray<TagComponent>* array = world ? world->getComponentArray<TagComponent>() : nullptr;
            int64_t count = 0;
            if (array) {
                for (size_t i = 0; i < array->size(); ++i) {
                    const TagComponent& tag = array->getComponentFromIndex(i);
                    if (wanted.empty() ? tag.hasTag() : tag.isTag(wanted)) {
                        count++;
                    }
                }
            }
            op::outInt(runtime, node, 0, count);
        }

        // ---- Text

        void textGet(GraphRuntime& runtime, const BoundNode& node) {
            TextComponent* text = op::component<TextComponent>(runtime, node, op::target(runtime, node, 0), "Text");
            op::outString(runtime, node, 0, text ? text->text : std::string());
        }

        int textSet(GraphRuntime& runtime, const BoundNode& node) {
            TextComponent* text = op::component<TextComponent>(runtime, node, op::target(runtime, node, 1), "Text");
            if (!text) {
                return 0;
            }
            const std::string value = op::text(runtime, node, 0);
            if (text->text != value) {
                text->text = value;
                text->needUpdateText = true;
            }
            return 0;
        }

        int textSetFontSize(GraphRuntime& runtime, const BoundNode& node) {
            TextComponent* text = op::component<TextComponent>(runtime, node, op::target(runtime, node, 1), "Text");
            if (!text) {
                return 0;
            }
            const int64_t size = op::integer(runtime, node, 0);
            const unsigned int wanted = size < 1 ? 1 : static_cast<unsigned int>(size);
            if (text->fontSize != wanted) {
                text->fontSize = wanted;
                text->needReloadAtlas = true;
                text->needUpdateText = true;
            }
            return 0;
        }

        // ---- Camera

        void cameraGet(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            op::outEntity(runtime, node, 0, world ? world->getCamera() : NULL_ENTITY);
        }

        int cameraSetActive(GraphRuntime& runtime, const BoundNode& node) {
            Scene* world = op::scene(runtime);
            const Entity camera = pinEntity(runtime, node, 0);
            if (!world || camera == NULL_ENTITY || !world->isEntityCreated(camera)) {
                op::warn(runtime, node, "target camera entity does not exist");
                return 0;
            }
            world->setCamera(camera);
            return 0;
        }

        // ---- Light

        void lightGetIntensity(GraphRuntime& runtime, const BoundNode& node) {
            LightComponent* light =
                op::component<LightComponent>(runtime, node, op::target(runtime, node, 0), "Light");
            op::outFloat(runtime, node, 0, light ? light->intensity : 0.0);
        }

        int lightSetIntensity(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 1);
            // Light::setIntensity also reads the Transform, so both must be there before the
            // wrapper runs - its getComponent() throws, and a throw is fatal here.
            LightComponent* light = op::component<LightComponent>(runtime, node, entity, "Light");
            if (!light || !op::component<Transform>(runtime, node, entity, "Transform")) {
                return 0;
            }
            Light wrapper(op::scene(runtime), entity);
            wrapper.setIntensity(static_cast<float>(op::number(runtime, node, 0)));
            return 0;
        }

        int lightSetColor(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 1);
            if (!op::component<LightComponent>(runtime, node, entity, "Light")) {
                return 0;
            }
            Light wrapper(op::scene(runtime), entity);
            wrapper.setColor(op::vector3(runtime, node, 0));
            return 0;
        }

        int lightSetRange(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 1);
            if (!op::component<LightComponent>(runtime, node, entity, "Light")) {
                return 0;
            }
            Light wrapper(op::scene(runtime), entity);
            wrapper.setRange(static_cast<float>(op::number(runtime, node, 0)));
            return 0;
        }

        int lightSetShadows(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 1);
            if (!op::component<LightComponent>(runtime, node, entity, "Light")) {
                return 0;
            }
            Light wrapper(op::scene(runtime), entity);
            wrapper.setShadows(op::flag(runtime, node, 0));
            return 0;
        }

        // ---- Particles

        int particlesPlay(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 0);
            ActionComponent* action = op::component<ActionComponent>(runtime, node, entity, "Action");
            if (action) {
                op::startAction(*action);
            }
            return 0;
        }

        int particlesStop(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 0);
            ActionComponent* action = op::component<ActionComponent>(runtime, node, entity, "Action");
            if (action) {
                op::stopAction(*action);
            }
            return 0;
        }

        int particlesReset(GraphRuntime& runtime, const BoundNode& node) {
            const Entity entity = op::target(runtime, node, 0);
            if (!op::component<ActionComponent>(runtime, node, entity, "Action") ||
                !op::component<ParticlesComponent>(runtime, node, entity, "Particles")) {
                return 0;
            }
            Particles(op::scene(runtime), entity).reset();
            return 0;
        }

        int particlesSetRate(GraphRuntime& runtime, const BoundNode& node) {
            ParticlesComponent* particles =
                op::component<ParticlesComponent>(runtime, node, op::target(runtime, node, 1), "Particles");
            if (!particles) {
                return 0;
            }
            const int64_t rate = op::integer(runtime, node, 0);
            particles->rate = rate < 0 ? 0 : static_cast<int>(rate);
            return 0;
        }

    }

    void registerEntityOps() {
        registerGraphOp("entity.self", BoundOpKind::Pure, nullptr, &entitySelf);
        registerGraphOp("entity.getPosition", BoundOpKind::Pure, nullptr, &entityGetPosition);
        registerGraphOp("entity.setPosition", BoundOpKind::Exec, &entitySetPosition, nullptr);
        registerGraphOp("entity.translate", BoundOpKind::Exec, &entityTranslate, nullptr);
        registerGraphOp("entity.move", BoundOpKind::Exec, &entityMove, nullptr);
        registerGraphOp("entity.getRotation", BoundOpKind::Pure, nullptr, &entityGetRotation);
        registerGraphOp("entity.setRotation", BoundOpKind::Exec, &entitySetRotation, nullptr);
        registerGraphOp("entity.setEulerRotation", BoundOpKind::Exec, &entitySetEulerRotation, nullptr);
        registerGraphOp("entity.rotate", BoundOpKind::Exec, &entityRotate, nullptr);
        registerGraphOp("entity.lookAt", BoundOpKind::Exec, &entityLookAt, nullptr);
        registerGraphOp("entity.getScale", BoundOpKind::Pure, nullptr, &entityGetScale);
        registerGraphOp("entity.setScale", BoundOpKind::Exec, &entitySetScale, nullptr);
        registerGraphOp("entity.scaleBy", BoundOpKind::Exec, &entityScaleBy, nullptr);
        registerGraphOp("entity.getParent", BoundOpKind::Pure, nullptr, &entityGetParent);
        registerGraphOp("entity.setParent", BoundOpKind::Exec, &entitySetParent, nullptr);
        registerGraphOp("entity.setVisible", BoundOpKind::Exec, &entitySetVisible, nullptr);
        registerGraphOp("entity.isVisible", BoundOpKind::Pure, nullptr, &entityIsVisible);
        registerGraphOp("entity.getName", BoundOpKind::Pure, nullptr, &entityGetName);
        registerGraphOp("entity.setName", BoundOpKind::Exec, &entitySetName, nullptr);
        registerGraphOp("entity.findByName", BoundOpKind::Pure, nullptr, &entityFindByName);
        registerGraphOp("entity.exists", BoundOpKind::Pure, nullptr, &entityExists);
        registerGraphOp("entity.count", BoundOpKind::Pure, nullptr, &entityCount);
        registerGraphOp("entity.destroy", BoundOpKind::Exec, &entityDestroy, nullptr);
        registerGraphOp("entity.spawn", BoundOpKind::Exec, &entitySpawn, nullptr);
        registerGraphOp("entity.distanceTo", BoundOpKind::Pure, nullptr, &entityDistanceTo);
        registerGraphOp("entity.directionTo", BoundOpKind::Pure, nullptr, &entityDirectionTo);
        registerGraphOp("entity.hasComponent", BoundOpKind::Pure, nullptr, &entityHasComponent);
        registerGraphOp("entity.setTag", BoundOpKind::Exec, &entitySetTag, nullptr);
        registerGraphOp("entity.getTag", BoundOpKind::Pure, nullptr, &entityGetTag);
        registerGraphOp("entity.hasTag", BoundOpKind::Pure, nullptr, &entityHasTag);
        registerGraphOp("entity.findByTag", BoundOpKind::Pure, nullptr, &entityFindByTag);
        registerGraphOp("entity.countTag", BoundOpKind::Pure, nullptr, &entityCountTag);

        registerGraphOp("text.get", BoundOpKind::Pure, nullptr, &textGet);
        registerGraphOp("text.set", BoundOpKind::Exec, &textSet, nullptr);
        registerGraphOp("text.setFontSize", BoundOpKind::Exec, &textSetFontSize, nullptr);

        registerGraphOp("camera.get", BoundOpKind::Pure, nullptr, &cameraGet);
        registerGraphOp("camera.setActive", BoundOpKind::Exec, &cameraSetActive, nullptr);

        registerGraphOp("light.getIntensity", BoundOpKind::Pure, nullptr, &lightGetIntensity);
        registerGraphOp("light.setIntensity", BoundOpKind::Exec, &lightSetIntensity, nullptr);
        registerGraphOp("light.setColor", BoundOpKind::Exec, &lightSetColor, nullptr);
        registerGraphOp("light.setRange", BoundOpKind::Exec, &lightSetRange, nullptr);
        registerGraphOp("light.setShadows", BoundOpKind::Exec, &lightSetShadows, nullptr);

        registerGraphOp("particles.play", BoundOpKind::Exec, &particlesPlay, nullptr);
        registerGraphOp("particles.stop", BoundOpKind::Exec, &particlesStop, nullptr);
        registerGraphOp("particles.reset", BoundOpKind::Exec, &particlesReset, nullptr);
        registerGraphOp("particles.setRate", BoundOpKind::Exec, &particlesSetRate, nullptr);
    }

}
}
