#include "gui_node_spine.h"
#include "res_spine_scene.h"
#include <common/vertices.h>

#include <dmsdk/dlib/buffer.h>
#include <dmsdk/dlib/dstrings.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gamesys/gui.h>
#include <dmsdk/script/script.h>

#include <spine/extension.h>
#include <spine/Skeleton.h>
#include <spine/Slot.h>
#include <spine/AnimationState.h>
#include <spine/Attachment.h>
#include <spine/RegionAttachment.h>
#include <spine/MeshAttachment.h>

#include "spine_ddf.h" // generated from the spine_ddf.proto
#include "script_spine_gui.h"

namespace dmSpine
{

static const dmhash_t SPINE_SCENE               = dmHashString64("spine_scene");
static const dmhash_t SPINE_DEFAULT_ANIMATION   = dmHashString64("spine_default_animation");
static const dmhash_t SPINE_SKIN                = dmHashString64("spine_skin");
static const dmhash_t SPINE_SCENE_SUFFIX        = dmHashString64(".spinescenec");

struct GuiNodeTypeContext
{
    // In case we need something later. Here for visibility
};

struct InternalGuiNode
{
    dmhash_t            m_SpinePath;
    SpineSceneResource* m_SpineScene;

    spSkeleton*         m_SkeletonInstance;
    spAnimationState*   m_AnimationStateInstance;
    spTrackEntry*       m_AnimationInstance;
    dmhash_t            m_AnimationId;
    dmhash_t            m_SkinId;

    dmVMath::Matrix4    m_Transform; // the world transform

    dmGui::Playback     m_Playback;
    dmGui::HScene       m_GuiScene;
    dmGui::HNode        m_GuiNode;
    dmGui::AdjustMode   m_AdjustMode;
    const char*         m_Id;

    dmArray<dmGui::HNode>   m_BonesNodes;
    dmArray<dmhash_t>       m_BonesIds; // Matches 1:1 with m_BoneNodes
    dmArray<spBone*>        m_Bones; // Matches 1:1 with m_BoneNodes

    dmScript::LuaCallbackInfo* m_Callback;

    uint8_t             m_Playing : 1;
    uint8_t             m_UseCursor : 1;
    uint8_t             : 6;

    InternalGuiNode()
    : m_SpinePath(0)
    , m_SpineScene(0)
    , m_SkeletonInstance(0)
    , m_AnimationStateInstance(0)
    , m_AnimationInstance(0)
    , m_AnimationId(0)
    , m_SkinId(0)
    , m_Callback(0)
    , m_Playing(0)
    , m_UseCursor(0)
    {}
};

static bool SetupNode(dmhash_t path, SpineSceneResource* resource, InternalGuiNode* node);

static inline bool IsLooping(dmGui::Playback playback)
{
    return  playback == dmGui::PLAYBACK_LOOP_BACKWARD ||
            playback == dmGui::PLAYBACK_LOOP_FORWARD ||
            playback == dmGui::PLAYBACK_LOOP_PINGPONG;
}

static inline bool IsReverse(dmGui::Playback playback)
{
    return  playback == dmGui::PLAYBACK_LOOP_BACKWARD ||
            playback == dmGui::PLAYBACK_ONCE_BACKWARD;
}

static inline bool IsPingPong(dmGui::Playback playback)
{
    return  playback == dmGui::PLAYBACK_LOOP_PINGPONG ||
            playback == dmGui::PLAYBACK_ONCE_PINGPONG;
}

// static void printStack(lua_State* L)
// {
//     int top = lua_gettop(L);
//     int bottom = 1;
//     lua_getglobal(L, "tostring");
//     for(int i = top; i >= bottom; i--)
//     {
//         lua_pushvalue(L, -1);
//         lua_pushvalue(L, i);
//         lua_pcall(L, 1, 1, 0);
//         const char *str = lua_tostring(L, -1);
//         if (str) {
//             printf("%2d: %s\n", i, str);
//         }else{
//             printf("%2d: %s\n", i, luaL_typename(L, i));
//         }
//         lua_pop(L, 1);
//     }
//     lua_pop(L, 1);
// }

static void SendDDF(InternalGuiNode* node, const dmDDF::Descriptor* descriptor, const char* data)
{
    if (!dmScript::IsCallbackValid(node->m_Callback))
        return;

    lua_State* L = dmScript::GetCallbackLuaContext(node->m_Callback);
    DM_LUA_STACK_CHECK(L, 0);

    if (!dmScript::SetupCallback(node->m_Callback))
    {
        dmLogError("Failed to setup callback");
        return;
    }

    dmGui::LuaPushNode(L, node->m_GuiScene, node->m_GuiNode);
    dmScript::PushHash(L, descriptor->m_NameHash);
    dmScript::PushDDF(L, descriptor, data, true); // from comp_script.cpp

    dmScript::PCall(L, 4, 0); // instance + 3

    dmScript::TeardownCallback(node->m_Callback);
}

static void SendAnimationDone(InternalGuiNode* node, const spAnimationState* state, const spTrackEntry* entry, const spEvent* event)
{
    dmGameSystemDDF::SpineAnimationDone message;
    message.m_AnimationId = dmHashString64(entry->animation->name);
    message.m_Playback    = node->m_Playback;
    message.m_Track       = entry->trackIndex;

    SendDDF(node, dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor, (const char*)&message);
}

static void SendSpineEvent(InternalGuiNode* node, const spAnimationState* state, const spTrackEntry* entry, const spEvent* event)
{
    dmGameSystemDDF::SpineEvent message;
    message.m_AnimationId = dmHashString64(entry->animation->name);
    message.m_EventId     = dmHashString64(event->data->name);
    message.m_BlendWeight = 0.0f;//keyframe_event->m_BlendWeight;
    message.m_T           = event->time;
    message.m_Integer     = event->intValue;
    message.m_Float       = event->floatValue;
    message.m_String      = dmHashString64(event->stringValue?event->stringValue:"");
    message.m_Node.m_Ref  = 0;
    message.m_Node.m_ContextTableRef = 0;

    SendDDF(node, dmGameSystemDDF::SpineEvent::m_DDFDescriptor, (const char*)&message);
}

static void SpineEventListener(spAnimationState* state, spEventType type, spTrackEntry* entry, spEvent* event)
{
    InternalGuiNode* node = (InternalGuiNode*)state->userData;

    switch (type)
    {
    // case SP_ANIMATION_START:
    //     printf("Animation %s started on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
    // case SP_ANIMATION_INTERRUPT:
    //     printf("Animation %s interrupted on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
    // case SP_ANIMATION_END:
    //     printf("Animation %s ended on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
    case SP_ANIMATION_COMPLETE:
        {
            //printf("Animation %s complete on track %i\n", entry->animation->name, entry->trackIndex);
    // TODO: Should we send event for looping animations as well?

            if (!IsLooping(node->m_Playback))
            {
                node->m_Playing = 0;

                if (node->m_Callback)
                {
                    // We only send the event if it's not looping (same behavior as before)
                    SendAnimationDone(node, state, entry, event);

                    dmScript::DestroyCallback(node->m_Callback); // The animation has ended, so we won't send any more on this
                    node->m_Callback = 0;
                }
            }

            if (IsPingPong(node->m_Playback))
            {
                node->m_AnimationInstance->reverse = !node->m_AnimationInstance->reverse;
            }
        }
        break;
    // case SP_ANIMATION_DISPOSE:
    //     printf("Track entry for animation %s disposed on track %i\n", entry->animation->name, entry->trackIndex);
    //     break;
    case SP_ANIMATION_EVENT:
        SendSpineEvent(node, state, entry, event);
        break;
    default:
        break;
    }
}

static const uint32_t INVALID_ANIMATION_INDEX = 0xFFFFFFFF;

static inline uint32_t FindAnimationIndex(InternalGuiNode* node, dmhash_t animation)
{
    SpineSceneResource* spine_scene = node->m_SpineScene;
    uint32_t* index = spine_scene->m_AnimationNameToIndex.Get(animation);
    return index ? *index : INVALID_ANIMATION_INDEX;
}

static bool PlayAnimation(InternalGuiNode* node, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, dmScript::LuaCallbackInfo* callback)
{
    SpineSceneResource* spine_scene = node->m_SpineScene;
    uint32_t index = FindAnimationIndex(node, animation_id);
    if (index == INVALID_ANIMATION_INDEX)
    {
        dmLogError("No animation '%s' found", dmHashReverseSafe64(animation_id));
        return false;
    }
    else if (index >= spine_scene->m_Skeleton->animationsCount)
    {
        dmLogError("Animation index %u is too large. Number of animations are %u", index, spine_scene->m_Skeleton->animationsCount);
        return false;
    }

    int trackIndex = 0;
    int loop = IsLooping(playback);

    spAnimation* animation = spine_scene->m_Skeleton->animations[index];

    node->m_AnimationId = animation_id;
    node->m_AnimationInstance = spAnimationState_setAnimation(node->m_AnimationStateInstance, trackIndex, animation, loop);

    node->m_Playing = 1;
    node->m_Playback = playback;
    node->m_UseCursor = 0;
    node->m_AnimationInstance->timeScale = playback_rate;
    node->m_AnimationInstance->reverse = IsReverse(playback);
    node->m_AnimationInstance->mixDuration = blend_duration;

    if (node->m_Callback)
    {
        dmScript::DestroyCallback(node->m_Callback);
    }
    node->m_Callback = callback;

    return true;
}

// SCRIPTING

bool SetScene(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t spine_scene)
{
    (void)hnode;
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);

    if (spine_scene == node->m_SpinePath)
        return true;

    SpineSceneResource* resource = (SpineSceneResource*)dmGui::GetResource(scene, spine_scene, SPINE_SCENE_SUFFIX);
    if (!resource)
        return false;

    if (node->m_AnimationStateInstance)
        spAnimationState_dispose(node->m_AnimationStateInstance);
    node->m_AnimationStateInstance = 0;
    if (node->m_SkeletonInstance)
        spSkeleton_dispose(node->m_SkeletonInstance);
    node->m_SkeletonInstance = 0;

    // if we want to play an animation, the user needs to explicitly do it with gui.play_spine_anim()
    // which will then ofc also use a callback
    // It in turn means that we have no use for the current callback
    dmScript::DestroyCallback(node->m_Callback);
    node->m_Callback = 0;

    return SetupNode(spine_scene, resource, node);
}

dmhash_t GetScene(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return node->m_SpinePath;
}

dmGui::HNode GetBone(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t bone_id)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    uint32_t count = node->m_BonesIds.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (node->m_BonesIds[i] == bone_id)
            return node->m_BonesNodes[i];
    }
    return 0;
}

bool PlayAnimation(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t animation_id, dmGui::Playback playback,
                            float blend_duration, float offset, float playback_rate, dmScript::LuaCallbackInfo* callback)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return PlayAnimation(node, animation_id, playback, blend_duration, offset, playback_rate, callback);
}

void CancelAnimation(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    node->m_Playing = 0;
}

bool SetSkin(dmGui::HScene scene, dmGui::HNode hnode, dmhash_t skin_id)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spSkin* skin = node->m_SpineScene->m_Skeleton->defaultSkin;
    if (skin_id)
    {
        uint32_t* index = node->m_SpineScene->m_SkinNameToIndex.Get(skin_id);
        if (!index)
        {
            return false;
        } else {
            skin = node->m_SpineScene->m_Skeleton->skins[*index];
        }
    }

    spSkeleton_setSkin(node->m_SkeletonInstance, skin);
    return true;
}

dmhash_t GetSkin(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return node->m_SkinId;
}

dmhash_t GetAnimation(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    return node->m_AnimationId;
}

bool SetCursor(dmGui::HScene scene, dmGui::HNode hnode, float cursor)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    if (!node->m_AnimationInstance)
    {
        return false;
    }

    float unit_0_1 = fmodf(cursor + 1.0f, 1.0f);

    float duration = node->m_AnimationInstance->animationEnd - node->m_AnimationInstance->animationStart;
    float t = unit_0_1 * duration;

    node->m_AnimationInstance->trackTime = t;
    node->m_UseCursor = 1;
    return true;
}

float GetCursor(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    spTrackEntry* entry = node->m_AnimationInstance;
    float unit = 0.0f;
    if (entry)
    {
        float duration = entry->animationEnd - entry->animationStart;
        if (duration != 0)
        {
            unit = fmodf(entry->trackTime, duration) / duration;
        }
    }
    return unit;
}

bool SetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode, float playback_rate)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    if (!node->m_AnimationInstance)
        return false;
    node->m_AnimationInstance->timeScale = playback_rate;
    return true;
}

float GetPlaybackRate(dmGui::HScene scene, dmGui::HNode hnode)
{
    InternalGuiNode* node = (InternalGuiNode*)dmGui::GetNodeCustomData(scene, hnode);
    if (!node->m_AnimationInstance)
        return 1.0f;
    return node->m_AnimationInstance->timeScale;
}

// END SCRIPTING

static void DeleteBones(InternalGuiNode* node)
{
    uint32_t count = node->m_BonesNodes.Size();
    for (uint32_t i = 0; i < count; ++i)
    {
        dmGui::DeleteNode(node->m_GuiScene, node->m_GuiNode);
    }
    node->m_BonesNodes.SetSize(0);
    node->m_BonesIds.SetSize(0);
    node->m_Bones.SetSize(0);
}

static void UpdateTransform(dmGui::HScene scene, dmGui::HNode node, const spBone* bone)
{
    float radians = spBone_getWorldRotationX((spBone*)bone);
    float sx = spBone_getWorldScaleX((spBone*)bone);
    float sy = spBone_getWorldScaleY((spBone*)bone);

    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_POSITION, dmVMath::Vector4(bone->worldX, bone->worldY, 0, 0));
    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_ROTATION, dmVMath::Vector4(0, 0, radians, 0));
    dmGui::SetNodeProperty(scene, node, dmGui::PROPERTY_SCALE, dmVMath::Vector4(sx, sy, 1, 0));
}

static dmGui::HNode CreateBone(dmGui::HScene scene, dmGui::HNode gui_parent, dmGui::AdjustMode adjust_mode, const char* spine_gui_node_id, spBone* bone)
{
    dmVMath::Point3 position = dmVMath::Point3(bone->x, bone->y, 0);
    dmGui::HNode gui_bone = dmGui::NewNode(scene, position, dmVMath::Vector3(0,0,0), dmGui::NODE_TYPE_BOX, 0);
    if (!gui_bone)
        return 0;

    char id_str[256];
    dmSnPrintf(id_str, sizeof(id_str), "%s/%s", spine_gui_node_id, bone->data->name);
    dmhash_t id = dmHashString64(id_str);

    dmGui::SetNodeId(scene, gui_bone, id);
    dmGui::SetNodeAdjustMode(scene, gui_bone, adjust_mode);
    dmGui::SetNodeParent(scene, gui_bone, gui_parent, false);
    dmGui::SetNodeIsBone(scene, gui_bone, true);

    UpdateTransform(scene, gui_bone, bone);

    return gui_bone;
}

static bool CreateBones(InternalGuiNode* node, dmGui::HScene scene, dmGui::HNode gui_parent, spBone* bone)
{
    dmGui::HNode gui_bone = CreateBone(scene, gui_parent, node->m_AdjustMode, node->m_Id, bone);
    if (!gui_bone)
        return false;

    node->m_BonesNodes.Push(gui_bone);
    node->m_BonesIds.Push(dmGui::GetNodeId(scene, gui_bone));
    node->m_Bones.Push(bone);

    int count = bone->childrenCount;
    for (int i = 0; i < count; ++i)
    {
        spBone* child_bone = bone->children[i];

        if (!CreateBones(node, scene, gui_parent, child_bone))
            return false;
    }
    return true;
}

static bool CreateBones(InternalGuiNode* node)
{
    DeleteBones(node);

    uint32_t num_bones = (uint32_t)node->m_SkeletonInstance->bonesCount;
    if (node->m_BonesNodes.Capacity() < num_bones)
    {
        node->m_BonesNodes.SetCapacity(num_bones);
        node->m_BonesIds.SetCapacity(num_bones);
        node->m_Bones.SetCapacity(num_bones);
    }

    return CreateBones(node, node->m_GuiScene, node->m_GuiNode, node->m_SkeletonInstance->root);
}

static void UpdateBones(InternalGuiNode* node)
{
    dmGui::HScene scene = node->m_GuiScene;
    uint32_t num_bones = node->m_BonesNodes.Size();

    dmVMath::Vector4 scale = dmGui::GetNodeProperty(scene, node->m_GuiNode, dmGui::PROPERTY_SCALE);
    for (uint32_t i = 0; i < num_bones; ++i)
    {
        spBone* bone = node->m_Bones[i];
        dmGui::HNode gui_bone = node->m_BonesNodes[i];

        UpdateTransform(scene, gui_bone, bone);
    }
}

static void DestroyNode(InternalGuiNode* node)
{
    DeleteBones(node);

    if (node->m_Callback)
    {
        dmScript::DestroyCallback(node->m_Callback);
    }

    if (node->m_AnimationStateInstance)
        spAnimationState_dispose(node->m_AnimationStateInstance);
    if (node->m_SkeletonInstance)
        spSkeleton_dispose(node->m_SkeletonInstance);

    //delete node; // don't delete it. It's already been registered with the comp_gui and we need to wait for the GuiDestroy
}

static void* GuiCreate(const dmGameSystem::CompGuiNodeContext* ctx, void* context, dmGui::HScene scene, dmGui::HNode node, uint32_t custom_type)
{
    InternalGuiNode* node_data = new InternalGuiNode();
    node_data->m_GuiScene = scene;
    node_data->m_GuiNode = node;
    return node_data;
}

static void GuiDestroy(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx)
{
    delete (InternalGuiNode*)(nodectx->m_NodeData);
}

static bool SetupNode(dmhash_t path, SpineSceneResource* resource, InternalGuiNode* node)
{
    node->m_SpinePath    = path;
    node->m_SpineScene   = resource;

    node->m_SkeletonInstance = spSkeleton_create(node->m_SpineScene->m_Skeleton);
    if (!node->m_SkeletonInstance)
    {
        dmLogError("%s: Failed to create skeleton instance", __FUNCTION__);
        DestroyNode(node);
        return false;
    }

    SetSkin(node->m_GuiScene, node->m_GuiNode, 0);

    node->m_AnimationStateInstance = spAnimationState_create(node->m_SpineScene->m_AnimationStateData);
    if (!node->m_AnimationStateInstance)
    {
        dmLogError("%s: Failed to create animation state instance", __FUNCTION__);
        DestroyNode(node);
        return false;
    }

    node->m_AnimationStateInstance->userData = node;
    node->m_AnimationStateInstance->listener = SpineEventListener;

    spSkeleton_setToSetupPose(node->m_SkeletonInstance);
    spSkeleton_updateWorldTransform(node->m_SkeletonInstance);

    node->m_Transform = dmVMath::Matrix4::identity();

    dmGui::SetNodeTexture(node->m_GuiScene, node->m_GuiNode, dmGui::NODE_TEXTURE_TYPE_TEXTURE_SET, node->m_SpineScene->m_TextureSet);

    CreateBones(node);

    return true;

}

static void* GuiClone(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx)
{
    InternalGuiNode* src = (InternalGuiNode*)nodectx->m_NodeData;
    InternalGuiNode* dst = new InternalGuiNode();

    dst->m_GuiScene = nodectx->m_Scene;
    dst->m_GuiNode = nodectx->m_Node;

    // We don't get a GuiSetNodeDesc call when cloning, as we should already have the data we need in the node itself
    dst->m_Id = src->m_Id;
    dst->m_AdjustMode = src->m_AdjustMode;
    dst->m_AnimationId = src->m_AnimationId;
    dst->m_SkinId = src->m_SkinId;

    // Setup the spine structures
    SetupNode(src->m_SpinePath, src->m_SpineScene, dst);

    // Now set the correct animation
    dst->m_Transform    = src->m_Transform;
    dst->m_Playback     = src->m_Playback;
    dst->m_Playing      = src->m_Playing;
    dst->m_UseCursor    = src->m_UseCursor;

    uint32_t index = FindAnimationIndex(dst, dst->m_AnimationId);
    if (index == INVALID_ANIMATION_INDEX)
    {
        dmLogError("No animation '%s' found", dmHashReverseSafe64(dst->m_AnimationId));
    }
    else if (index >= dst->m_SpineScene->m_Skeleton->animationsCount)
    {
        dmLogError("Animation index %u is too large. Number of animations are %u", index, dst->m_SpineScene->m_Skeleton->animationsCount);
        index = INVALID_ANIMATION_INDEX;
    }

    if (index != INVALID_ANIMATION_INDEX)
    {
        spAnimation* animation = dst->m_SpineScene->m_Skeleton->animations[index];
        if (animation)
        {
            int trackIndex = 0;
            int loop = IsLooping(dst->m_Playback);
            dst->m_AnimationId = src->m_AnimationId;
            dst->m_AnimationInstance = spAnimationState_setAnimation(dst->m_AnimationStateInstance, trackIndex, animation, loop);

            // Now copy the state of the animation
            dst->m_AnimationInstance->trackTime = src->m_AnimationInstance->trackTime;
            dst->m_AnimationInstance->reverse = src->m_AnimationInstance->reverse;
            dst->m_AnimationInstance->timeScale = src->m_AnimationInstance->timeScale;
        }
    }

    return dst;
}

static void GuiSetNodeDesc(const dmGameSystem::CompGuiNodeContext* ctx, const dmGameSystem::CustomNodeCtx* nodectx, const dmGuiDDF::NodeDesc* node_desc)
{
    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);

    dmhash_t name_hash = dmHashString64(node_desc->m_SpineScene);
    SpineSceneResource* resource = (SpineSceneResource*)dmGui::GetResource(nodectx->m_Scene, name_hash, SPINE_SCENE_SUFFIX);
    if (!resource) {
        dmLogError("Failed to get resource: %s", node_desc->m_SpineScene);
        return;
    }

    node->m_Id = node_desc->m_Id;
    node->m_AdjustMode = (dmGui::AdjustMode)node_desc->m_AdjustMode;
    node->m_AnimationId = dmHashString64(node_desc->m_SpineDefaultAnimation); // TODO: Q: Is the default playmode specified anywhere?
    node->m_SkinId = dmHashString64(node_desc->m_SpineSkin);

    SetupNode(name_hash, resource, node);

    if (node->m_SkinId) {
        SetSkin(node->m_GuiScene, node->m_GuiNode, node->m_SkinId);
    }

    if (node->m_AnimationId) {
        PlayAnimation(node, node->m_AnimationId, dmGui::PLAYBACK_LOOP_FORWARD, 0.0f, 0.0f, 1.0f, 0);
    }
}

static void GuiGetVertices(const dmGameSystem::CustomNodeCtx* nodectx, uint32_t decl_size, dmBuffer::StreamDeclaration* decl, uint32_t struct_size, dmArray<uint8_t>& vertices)
{
    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);

    //TODO: Verify the vertex declaration
    // In theory, we can check the vertex format to see which components to output
    // We currently know it's xyz-uv-rgba
    dmArray<dmSpine::SpineVertex>* vbdata = (dmArray<dmSpine::SpineVertex>*)&vertices;

    uint32_t num_vertices = dmSpine::GenerateVertexData(*vbdata, node->m_SkeletonInstance, node->m_Transform);
    (void)num_vertices;
}

static void GuiUpdate(const dmGameSystem::CustomNodeCtx* nodectx, float dt)
{
    InternalGuiNode* node = (InternalGuiNode*)(nodectx->m_NodeData);
    if (!node->m_AnimationStateInstance)
        return;

    float anim_dt = node->m_UseCursor ? 0.0f : dt;
    node->m_UseCursor = 0;

    if (node->m_Playing)
    {
        spAnimationState_update(node->m_AnimationStateInstance, anim_dt);
        spAnimationState_apply(node->m_AnimationStateInstance, node->m_SkeletonInstance);
    }

    spSkeleton_updateWorldTransform(node->m_SkeletonInstance);

    UpdateBones(node);
}

static dmGameObject::Result GuiNodeTypeSpineCreate(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    GuiNodeTypeContext* type_context = new GuiNodeTypeContext;
    dmGameSystem::CompGuiNodeTypeSetContext(type, type_context);

    dmGameSystem::CompGuiNodeTypeSetCreateFn(type, GuiCreate);
    dmGameSystem::CompGuiNodeTypeSetDestroyFn(type, GuiDestroy);
    dmGameSystem::CompGuiNodeTypeSetCloneFn(type, GuiClone);
    dmGameSystem::CompGuiNodeTypeSetUpdateFn(type, GuiUpdate);
    dmGameSystem::CompGuiNodeTypeSetGetVerticesFn(type, GuiGetVertices);
    dmGameSystem::CompGuiNodeTypeSetNodeDescFn(type, GuiSetNodeDesc);

    lua_State* L = dmGameSystem::GetLuaState(ctx);
    ScriptSpineGuiRegister(L);

    return dmGameObject::RESULT_OK;
}

static dmGameObject::Result GuiNodeTypeSpineDestroy(const dmGameSystem::CompGuiNodeTypeCtx* ctx, dmGameSystem::CompGuiNodeType* type)
{
    GuiNodeTypeContext* type_context = (GuiNodeTypeContext*)dmGameSystem::CompGuiNodeTypeGetContext(type);
    delete type_context;
    return dmGameObject::RESULT_OK;
}

} // namespace



DM_DECLARE_COMPGUI_NODE_TYPE(ComponentTypeGuiNodeSpineModelExt, "Spine", dmSpine::GuiNodeTypeSpineCreate, dmSpine::GuiNodeTypeSpineDestroy)
