/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2012-2018 DragonBones team and other contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef DRAGONBONES_ANIMATION_STATE_H
#define DRAGONBONES_ANIMATION_STATE_H

#include "../core/BaseObject.h"
#include "../geom/Transform.h"
#include <utility>

DRAGONBONES_NAMESPACE_BEGIN
/**
 * - The animation state is generated when the animation data is played.
 * @see dragonBones.Animation
 * @see dragonBones.AnimationData
 * @version DragonBones 3.0
 * @language en_US
 */
/**
 * - 动画状态由播放动画数据时产生。
 * @see dragonBones.Animation
 * @see dragonBones.AnimationData
 * @version DragonBones 3.0
 * @language zh_CN
 */
class AnimationState : public BaseObject
{
    BIND_CLASS_TYPE_B(AnimationState);

private:
    enum class BaseTimelineType {
        Bone,
        Slot,
        Constraint
    };

public:
    /**
     * @private
     */
    bool actionEnabled;
    /**
     * @private
     */
    bool additiveBlending;
    /**
     * - Whether the animation state has control over the display object properties of the slots.
     * Sometimes blend a animation state does not want it to control the display object properties of the slots,
     * especially if other animation state are controlling the display object properties of the slots.
     * @default true
     * @version DragonBones 5.0
     * @language en_US
     */
    /**
     * - 动画状态是否对插槽的显示对象属性有控制权。
     * 有时混合一个动画状态并不希望其控制插槽的显示对象属性，
     * 尤其是其他动画状态正在控制这些插槽的显示对象属性时。
     * @default true
     * @version DragonBones 5.0
     * @language zh_CN
     */
    bool displayControl;
    /**
     * - Whether to reset the objects without animation to the armature pose when the animation state is start to play.
     * This property should usually be set to false when blend multiple animation states.
     * @default true
     * @version DragonBones 5.1
     * @language en_US
     */
    /**
     * - 开始播放动画状态时是否将没有动画的对象重置为骨架初始值。
     * 通常在混合多个动画状态时应该将该属性设置为 false。
     * @default true
     * @version DragonBones 5.1
     * @language zh_CN
     */
    bool resetToPose;
    /**
     * - The play times. [0: Loop play, [1~N]: Play N times]
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 播放次数。 [0: 无限循环播放, [1~N]: 循环播放 N 次]
     * @version DragonBones 3.0
     * @language zh_CN
     */
    unsigned playTimes;
    /**
     * - The blend layer.
     * High layer animation state will get the blend weight first.
     * When the blend weight is assigned more than 1, the remaining animation states will no longer get the weight assigned.
     * @readonly
     * @version DragonBones 5.0
     * @language en_US
     */
    /**
     * - 混合图层。
     * 图层高的动画状态会优先获取混合权重。
     * 当混合权重分配超过 1 时，剩余的动画状态将不再获得权重分配。
     * @readonly
     * @version DragonBones 5.0
     * @language zh_CN
     */
    unsigned layer;
    /**
     * - The play speed.
     * The value is an overlay relationship with {@link dragonBones.Animation#timeScale}.
     * [(-N~0): Reverse play, 0: Stop play, (0~1): Slow play, 1: Normal play, (1~N): Fast play]
     * @default 1.0
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 播放速度。
     * 该值与 {@link dragonBones.Animation#timeScale} 是叠加关系。
     * [(-N~0): 倒转播放, 0: 停止播放, (0~1): 慢速播放, 1: 正常播放, (1~N): 快速播放]
     * @default 1.0
     * @version DragonBones 3.0
     * @language zh_CN
     */
    float timeScale;
    /**
     * - The blend weight.
     * @default 1.0
     * @version DragonBones 5.0
     * @language en_US
     */
    /**
     * - 混合权重。
     * @default 1.0
     * @version DragonBones 5.0
     * @language zh_CN
     */
    float weight;
    /**
     * - The auto fade out time when the animation state play completed.
     * [-1: Do not fade out automatically, [0~N]: The fade out time] (In seconds)
     * @default -1.0
     * @version DragonBones 5.0
     * @language en_US
     */
    /**
     * - 动画状态播放完成后的自动淡出时间。
     * [-1: 不自动淡出, [0~N]: 淡出时间] （以秒为单位）
     * @default -1.0
     * @version DragonBones 5.0
     * @language zh_CN
     */
    float autoFadeOutTime;
    /**
     * @private
     */
    float fadeTotalTime;
    /**
     * - The name of the animation state. (Can be different from the name of the animation data)
     * @readonly
     * @version DragonBones 5.0
     * @language en_US
     */
    /**
     * - 动画状态名称。 （可以不同于动画数据）
     * @readonly
     * @version DragonBones 5.0
     * @language zh_CN
     */
    std::string name;
    /**
     * - The blend group name of the animation state.
     * This property is typically used to specify the substitution of multiple animation states blend.
     * @readonly
     * @version DragonBones 5.0
     * @language en_US
     */
    /**
     * - 混合组名称。
     * 该属性通常用来指定多个动画状态混合时的相互替换关系。
     * @readonly
     * @version DragonBones 5.0
     * @language zh_CN
     */
    std::string group;

public:
    /**
     * - xx: Play Enabled, Fade Play Enabled
     * @internal
     */
    int _playheadState;
    /**
     * -1: Fade in, 0: Fade complete, 1: Fade out;
     * @internal
     */
    int _fadeState;
    /**
     * -1: Fade start, 0: Fading, 1: Fade complete;
     * @internal
     */
    int _subFadeState;
    /**
     * @internal
     */
    float _position;
    /**
     * @internal
     */
    float _duration;
    /**
     * @internal
     */
    float _fadeProgress;
    /**
     * @internal
     */
    float _weightResult;
    /**
     * @internal
     */
    AnimationData* _animationData;
    /**
     * @internal
     */
    ActionTimelineState* _actionTimeline;
    /**
     * @internal
     * Phase synchronization state.
     */
    AnimationState* _syncLeader = nullptr;
    /**
     * @internal
     */
    std::vector<AnimationState*> _syncFollowers;
    /**
     * @internal
     */
    std::vector<std::pair<float, float>> _syncMarkers;
    /**
     * @internal
     * -1: reverse, 1: forward, 0: not moving; tells the action timeline how to dispatch frame events.
     */
    int _syncDirection = 1;
    /**
     * @internal
     * Pending crossing produced by the playhead pass, consumed (events replayed) in the timeline pass.
     */
    bool _syncCrossingPending = false;
    float _syncOldTime = 0.0f;
    float _syncNewTime = 0.0f;
    /**
     * @internal
     */
    bool _syncSuppressFrameEvents = false;
    /**
     * @internal
     * Per-frame marker used by Animation to update sync chains in leader-first order.
     */
    bool _syncUpdated = false;
    /**
     * @internal
     * The raw passed time of the current frame, shared by the playhead and timeline passes.
     */
    float _framePassedTime = 0.0f;

private:
    unsigned _timelineDirty;
    float _fadeTime;
    float _time;
    std::vector<std::string> _boneMask;
    std::vector<BoneTimelineState*> _boneTimelines;
    std::vector<SlotTimelineState*> _slotTimelines;
    std::vector<ConstraintTimelineState*> _constraintTimelines;
    std::vector<std::pair<TimelineState*, BaseTimelineType>> _poseTimelines;
    std::map<std::string, BonePose*> _bonePoses;
    Armature* _armature;
    ZOrderTimelineState* _zOrderTimeline;

public:
    AnimationState() :
        _actionTimeline(nullptr),
        _zOrderTimeline(nullptr)
    {
        _onClear();
    }
    virtual ~AnimationState()
    {
        _onClear();
    }

protected:
    virtual void _onClear() override;

private:
    void _updateTimelines();
    void _updateBoneAndSlotTimelines();
    void _advanceFadeTime(float passedTime);
    bool _validateSyncMarkers(const std::vector<std::pair<float, float>>& markers, float leaderDuration) const;
    float _mapSyncTime(float leaderTime) const;
    void _applySynchronizedTime();
    void _clearSyncBinding();

public:
    /**
     * @internal
     */
    void init(Armature* armature, AnimationData* animationData, AnimationConfig* animationConfig);
    /**
     * @internal
     */
    void advanceTime(float passedTime, float cacheFrameRate);
    /**
     * @internal
     * First pass: advance only the playhead time (or map the leader's phase), without touching timelines.
     */
    void _advancePlayhead(float passedTime);
    /**
     * @internal
     * Second pass: fade, timeline sampling and bone/slot blending, in the state list order.
     */
    void _advanceTimelines(float cacheFrameRate);
    /**
     * @internal
     */
    void _detachSyncOnRemove();
    /**
     * - Continue play.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 继续播放。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    void play();
    /**
     * - Stop play.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 暂停播放。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    void stop();
    /**
     * - Fade out the animation state.
     * @param fadeOutTime - The fade out time. (In seconds)
     * @param pausePlayhead - Whether to pause the animation playing when fade out.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 淡出动画状态。
     * @param fadeOutTime - 淡出时间。 （以秒为单位）
     * @param pausePlayhead - 淡出时是否暂停播放。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    void fadeOut(float fadeOutTime, bool pausePlayhead = true);
    /**
     * - Check if a specific bone mask is included.
     * @param boneName - The bone name.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 检查是否包含特定骨骼遮罩。
     * @param boneName - 骨骼名称。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    bool containsBoneMask(const std::string& boneName) const;
    /**
     * - Add a specific bone mask.
     * @param boneName - The bone name.
     * @param recursive - Whether or not to add a mask to the bone's sub-bone.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 添加特定的骨骼遮罩。
     * @param boneName - 骨骼名称。
     * @param recursive - 是否为该骨骼的子骨骼添加遮罩。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    void addBoneMask(const std::string& boneName, bool recursive = true);
    /**
     * - Remove the mask of a specific bone.
     * @param boneName - The bone name.
     * @param recursive - Whether to remove the bone's sub-bone mask.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 删除特定骨骼的遮罩。
     * @param boneName - 骨骼名称。
     * @param recursive - 是否删除该骨骼的子骨骼遮罩。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    void removeBoneMask(const std::string& boneName, bool recursive = true);
    /**
     * - Remove all bone masks.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 删除所有骨骼遮罩。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    void removeAllBoneMask();
    /**
     * - Whether the animation state is fading in.
     * @version DragonBones 5.1
     * @language en_US
     */
    /**
     * - 是否正在淡入。
     * @version DragonBones 5.1
     * @language zh_CN
     */
    inline bool isFadeIn() const 
    {
        return _fadeState < 0;
    }
    /**
     * - Whether the animation state is fading out.
     * @version DragonBones 5.1
     * @language en_US
     */
    /**
     * - 是否正在淡出。
     * @version DragonBones 5.1
     * @language zh_CN
     */
    inline bool isFadeOut() const
    {
        return _fadeState > 0;
    }
    /**
     * - Whether the animation state is fade completed.
     * @version DragonBones 5.1
     * @language en_US
     */
    /**
     * - 是否淡入或淡出完毕。
     * @version DragonBones 5.1
     * @language zh_CN
     */
    inline bool isFadeComplete() const
    {
        return _fadeState == 0;
    }
    /**
     * - Whether the animation state is playing.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 是否正在播放。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    bool isPlaying() const;
    /**
     * - Whether the animation state is play completed.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 是否播放完毕。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    bool isCompleted() const;
    /**
     * - The times has been played.
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 已经循环播放的次数。
     * @version DragonBones 3.0
     * @language zh_CN
     */
    unsigned getCurrentPlayTimes() const;
    /**
     * - The total time. (In seconds)
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 总播放时间。 （以秒为单位）
     * @version DragonBones 3.0
     * @language zh_CN
     */
    inline float getTotalTime() const
    {
        return _duration;
    }
    /**
     * - The time is currently playing. (In seconds)
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 当前播放的时间。 （以秒为单位）
     * @version DragonBones 3.0
     * @language zh_CN
     */
    float getCurrentTime() const;
    void setCurrentTime(float value);
    /**
     * @internal
     * The effective playhead direction, which for a synchronized state comes from the leader.
     */
    inline int _getPlayDirection() const
    {
        return _syncLeader != nullptr ? _syncDirection : (timeScale < 0.0f ? -1 : 1);
    }
    /**
     * - Bind this animation state as a follower of another active state in the same armature.
     * The follower's playhead keeps tracking the leader's phase, mapped piecewise linearly
     * through the marker pairs (leader time in seconds, follower time in seconds).
     * Only full-duration, infinitely looping (playTimes == 0) states can be bound.
     * @param leader - The leading animation state.
     * @param markers - Marker pairs, first must be (0, 0), last the full durations, each column strictly increasing.
     * @returns true when the binding was created (or replaced), false when any argument or state is invalid;
     * an existing binding is left untouched on failure.
     * @version DragonBones 5.7
     * @language en_US
     */
    /**
     * - 将本动画状态绑定为同一骨架内另一个活动状态的从属状态。
     * 从属状态的播放头按标记对（源动作秒数, 从属动作秒数）分段线性映射，持续跟随源状态的相位。
     * 只有播放完整动作且无限循环（playTimes == 0）的状态可以绑定。
     * @param leader - 作为相位源的动画状态。
     * @param markers - 标记对，首对必须为 (0, 0)，末对为两者完整时长，两列各自严格递增。
     * @returns 绑定成功（或替换成功）返回 true；参数或状态非法时返回 false 且原有绑定不变。
     * @version DragonBones 5.7
     * @language zh_CN
     */
    bool synchronizeTo(AnimationState* leader, const std::vector<std::pair<float, float>>& markers);
    /**
     * - Remove the phase synchronization binding. The state resumes playing with its own timeScale
     * from the last mapped time.
     * @version DragonBones 5.7
     * @language en_US
     */
    /**
     * - 解除相位同步绑定。该状态从最后一次实际映射到的时间开始，恢复按自身 timeScale 播放。
     * @version DragonBones 5.7
     * @language zh_CN
     */
    void unsynchronize();
    /**
     * - Whether this state is currently following another state's phase.
     * @version DragonBones 5.7
     * @language en_US
     */
    inline bool isSynchronized() const
    {
        return _syncLeader != nullptr;
    }
    /**
     * - The phase leader of this state, or null when it is not synchronized.
     * @version DragonBones 5.7
     * @language en_US
     */
    inline AnimationState* getSyncLeader() const
    {
        return _syncLeader;
    }
    inline const std::string& getName() const
    {
        return name;
    }

    /**
     * - The animation data.
     * @see dragonBones.AnimationData
     * @version DragonBones 3.0
     * @language en_US
     */
    /**
     * - 动画数据。
     * @see dragonBones.AnimationData
     * @version DragonBones 3.0
     * @language zh_CN
     */
    inline const AnimationData* getAnimationData() const
    { 
        return _animationData; 
    }
};
/**
 * @internal
 */
class BonePose : public BaseObject
{
    BIND_CLASS_TYPE_A(BonePose);

public:
    Transform current;
    Transform delta;
    Transform result;

protected:
    virtual void _onClear() override;
};
/**
 * @internal
 */
class BlendState
{
public:
    bool dirty;
    int layer;
    float leftWeight;
    float layerWeight;
    float blendWeight;

    /**
     * -1: First blending, 0: No blending, 1: Blending.
     */
    int update(float weight, int p_layer);
    void clear();
};

DRAGONBONES_NAMESPACE_END
#endif  // DRAGONBONES_ANIMATION_STATE_H
