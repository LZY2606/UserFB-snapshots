#include "AnimationState.h"
#include "Animation.h"
#include "WorldClock.h"
#include "../model/DisplayData.h"
#include "../model/AnimationConfig.h"
#include "../model/AnimationData.h"
#include "../armature/Armature.h"
#include "../armature/Bone.h"
#include "../armature/Slot.h"
#include "../armature/Constraint.h"
#include "../event/EventObject.h"
#include "../event/IEventDispatcher.h"
#include "TimelineState.h"

DRAGONBONES_NAMESPACE_BEGIN

void AnimationState::_onClear()
{
    _detachSyncOnRemove();

    for (const auto timeline : _boneTimelines)
    {
        timeline->returnToPool();
    }

    for (const auto timeline : _slotTimelines)
    {
        timeline->returnToPool();
    }

    for (const auto timeline : _constraintTimelines)
    {
        timeline->returnToPool();
    }

    for (const auto& pair : _bonePoses)
    {
        pair.second->returnToPool();
    }

    if (_actionTimeline !=  nullptr) {
        _actionTimeline->returnToPool();
    }

    if (_zOrderTimeline != nullptr) {
        _zOrderTimeline->returnToPool();
    }

    actionEnabled = false;
    additiveBlending = false;
    displayControl = false;
    resetToPose = false;
    playTimes = 1;
    layer = 0;
    timeScale = 1.0f;
    weight = 1.0f;
    autoFadeOutTime = 0.0f;
    fadeTotalTime = 0.0f;
    name = "";
    group = "";

    _timelineDirty = 2;
    _playheadState = 0;
    _fadeState = -1;
    _subFadeState = -1;
    _position = 0.0f;
    _duration = 0.0f;
    _fadeTime = 0.0f;
    _time = 0.0f;
    _fadeProgress = 0.0f;
    _weightResult = 0.0f;
    _boneMask.clear();
    _boneTimelines.clear();
    _slotTimelines.clear();
    _constraintTimelines.clear();
    _poseTimelines.clear();
    _bonePoses.clear();
    _animationData = nullptr;
    _armature = nullptr;
    _actionTimeline = nullptr;
    _zOrderTimeline = nullptr;
    _syncLeader = nullptr;
    _syncFollowers.clear();
    _syncMarkers.clear();
    _syncDirection = 1;
    _syncCrossingPending = false;
    _syncOldTime = 0.0f;
    _syncNewTime = 0.0f;
    _syncSuppressFrameEvents = false;
    _syncUpdated = false;
    _framePassedTime = 0.0f;
}

void AnimationState::_updateTimelines()
{
    { // Update constraint timelines.
        std::map<std::string, std::vector<ConstraintTimelineState*>> constraintTimelines;
        for (const auto timeline : _constraintTimelines) // Create constraint timelines map.
        {
            constraintTimelines[timeline->constraint->getName()].push_back(timeline);
        }

        for (const auto constraint : _armature->_constraints) 
        {
            const auto& timelineName = constraint->getName();
            const auto timelineDatas = _animationData->getConstraintTimelines(timelineName);
            const auto iterator = constraintTimelines.find(timelineName);

            if (iterator != constraintTimelines.end()) // Remove constraint timeline from map.
            {
                constraintTimelines.erase(iterator);
            }
            else  // Create new constraint timeline.
            {
                if (timelineDatas != nullptr) 
                {
                    for (const auto timelineData : *timelineDatas) 
                    {
                        switch (timelineData->type) 
                        {
                            case TimelineType::IKConstraint:
                            {
                                const auto timeline = BaseObject::borrowObject<IKConstraintTimelineState>();
                                timeline->constraint = constraint;
                                timeline->init(_armature, this, timelineData);
                                _constraintTimelines.push_back(timeline);
                                break;
                            }

                            default:
                                break;
                        }
                    }
                }
                else if (resetToPose) // Pose timeline.
                {
                    const auto timeline = BaseObject::borrowObject<IKConstraintTimelineState>();
                    timeline->constraint = constraint;
                    timeline->init(_armature, this, nullptr);
                    _constraintTimelines.push_back(timeline);
                    _poseTimelines.push_back(std::make_pair(timeline, BaseTimelineType::Constraint));
                }
            }
        }
    }
}

void AnimationState::_updateBoneAndSlotTimelines()
{
    { // Update bone timelines.
        std::map<std::string, std::vector<BoneTimelineState*>> boneTimelines;
        for (const auto timeline : _boneTimelines) // Create bone timelines map.
        {
            boneTimelines[timeline->bone->getName()].push_back(timeline);
        }

        for (const auto bone : _armature->getBones())
        {
            const auto& timelineName = bone->getName();
            if (!containsBoneMask(timelineName))
            {
                continue;
            }

            const auto timelineDatas = _animationData->getBoneTimelines(timelineName);
            const auto iterator = boneTimelines.find(timelineName);

            if (iterator != boneTimelines.end()) // Remove bone timeline from map.
            {
                boneTimelines.erase(iterator);
            }
            else // Create new bone timeline.
            {
                const auto bonePose = _bonePoses.find(timelineName) != _bonePoses.end() ? _bonePoses[timelineName] : (_bonePoses[timelineName] = BaseObject::borrowObject<BonePose>());

                if (timelineDatas != nullptr)
                {
                    for (const auto timelineData : *timelineDatas)
                    {
                        switch (timelineData->type)
                        {
                            case TimelineType::BoneAll:
                            {
                                const auto timeline = BaseObject::borrowObject<BoneAllTimelineState>();
                                timeline->bone = bone;
                                timeline->bonePose = bonePose;
                                timeline->init(_armature, this, timelineData);
                                _boneTimelines.push_back(timeline);
                                break;
                            }

                            case TimelineType::BoneTranslate:
                            {
                                const auto timeline = BaseObject::borrowObject<BoneTranslateTimelineState>();
                                timeline->bone = bone;
                                timeline->bonePose = bonePose;
                                timeline->init(_armature, this, timelineData);
                                _boneTimelines.push_back(timeline);
                                break;
                            }

                            case TimelineType::BoneRotate:
                            {
                                const auto timeline = BaseObject::borrowObject<BoneRotateTimelineState>();
                                timeline->bone = bone;
                                timeline->bonePose = bonePose;
                                timeline->init(_armature, this, timelineData);
                                _boneTimelines.push_back(timeline);
                                break;
                            }

                            case TimelineType::BoneScale:
                            {
                                const auto timeline = BaseObject::borrowObject<BoneScaleTimelineState>();
                                timeline->bone = bone;
                                timeline->bonePose = bonePose;
                                timeline->init(_armature, this, timelineData);
                                _boneTimelines.push_back(timeline);
                                break;
                            }

                            default:
                                break;
                            }
                    }
                }
                else if (resetToPose) // Pose timeline.
                {
                    const auto timeline = BaseObject::borrowObject<BoneAllTimelineState>();
                    timeline->bone = bone;
                    timeline->bonePose = bonePose;
                    timeline->init(_armature, this, nullptr);
                    _boneTimelines.push_back(timeline);
                    _poseTimelines.push_back(std::make_pair(timeline, BaseTimelineType::Bone));
                }
            }
        }

        for (const auto& pair : boneTimelines) // Remove bone timelines.
        {
            for (const auto timeline : pair.second)
            {
                _boneTimelines.erase(std::find(_boneTimelines.begin(), _boneTimelines.end(), timeline));
                timeline->returnToPool();
            }
        }
    }

    { // Update slot timelines.
        std::map<std::string, std::vector<SlotTimelineState*>> slotTimelines;
        std::vector<unsigned> ffdFlags;
        for (const auto timeline : _slotTimelines) // Create slot timelines map.
        {
            slotTimelines[timeline->slot->getName()].push_back(timeline);
        }

        for (const auto slot : _armature->getSlots())
        {
            const auto& boneName = slot->getParent()->getName();
            if (!containsBoneMask(boneName))
            {
                continue;
            }

            const auto& timelineName = slot->getName();
            const auto timelineDatas = _animationData->getSlotTimelines(timelineName);
            const auto iterator = slotTimelines.find(timelineName);

            if (iterator != slotTimelines.end()) // Remove slot timeline from map.
            {
                slotTimelines.erase(iterator);
            }
            else // Create new slot timeline.
            {
                auto displayIndexFlag = false;
                auto colorFlag = false;
                ffdFlags.clear();

                if (timelineDatas != nullptr)
                {
                    for (const auto timelineData : *timelineDatas)
                    {
                        switch (timelineData->type)
                        {
                            case TimelineType::SlotDisplay:
                            {
                                const auto timeline = BaseObject::borrowObject<SlotDislayTimelineState>();
                                timeline->slot = slot;
                                timeline->init(_armature, this, timelineData);
                                _slotTimelines.push_back(timeline);
                                displayIndexFlag = true;
                                break;
                            }

                            case TimelineType::SlotColor:
                            {
                                const auto timeline = BaseObject::borrowObject<SlotColorTimelineState>();
                                timeline->slot = slot;
                                timeline->init(_armature, this, timelineData);
                                _slotTimelines.push_back(timeline);
                                colorFlag = true;
                                break;
                            }

                            case TimelineType::SlotDeform:
                            {
                                const auto timeline = BaseObject::borrowObject<DeformTimelineState>();
                                timeline->slot = slot;
                                timeline->init(_armature, this, timelineData);
                                _slotTimelines.push_back(timeline);
                                ffdFlags.push_back(timeline->vertexOffset);
                                break;
                            }

                            default:
                                break;
                            }
                    }
                }

                if (resetToPose) // Pose timeline.
                {
                    if (!displayIndexFlag)
                    {
                        const auto timeline = BaseObject::borrowObject<SlotDislayTimelineState>();
                        timeline->slot = slot;
                        timeline->init(_armature, this, nullptr);
                        _slotTimelines.push_back(timeline);
                        _poseTimelines.push_back(std::make_pair(timeline, BaseTimelineType::Slot));
                    }

                    if (!colorFlag)
                    {
                        const auto timeline = BaseObject::borrowObject<SlotColorTimelineState>();
                        timeline->slot = slot;
                        timeline->init(_armature, this, nullptr);
                        _slotTimelines.push_back(timeline);
                        _poseTimelines.push_back(std::make_pair(timeline, BaseTimelineType::Slot));
                    }

                    if (slot->getRawDisplayDatas() != nullptr)
                    {
                        for (const auto displayData : *(slot->getRawDisplayDatas()))
                        {
                            if (displayData != nullptr && displayData->type == DisplayType::Mesh)
                            {
                                const auto meshOffset = static_cast<MeshDisplayData*>(displayData)->vertices.offset;
                                if (std::find(ffdFlags.cbegin(), ffdFlags.cend(), meshOffset) == ffdFlags.cend())
                                {
                                    const auto timeline = BaseObject::borrowObject<DeformTimelineState>();
                                    timeline->vertexOffset = meshOffset;
                                    timeline->slot = slot;
                                    timeline->init(_armature, this, nullptr);
                                    _slotTimelines.push_back(timeline);
                                    _poseTimelines.push_back(std::make_pair(timeline, BaseTimelineType::Slot));
                                }
                            }
                        }
                    }
                }
            }
        }

        for (const auto& pair : slotTimelines) // Remove slot timelines.
        {
            for (const auto timeline : pair.second)
            {
                _slotTimelines.erase(std::find(_slotTimelines.begin(), _slotTimelines.end(), timeline));
                timeline->returnToPool();
            }
        }
    }
}

void AnimationState::_advanceFadeTime(float passedTime)
{
    const auto isFadeOut = _fadeState > 0;

    if (_subFadeState < 0)
    {
        _subFadeState = 0;

        const auto eventType = isFadeOut ? EventObject::FADE_OUT : EventObject::FADE_IN;
        if (_armature->getProxy()->hasDBEventListener(eventType))
        {
            const auto eventObject = BaseObject::borrowObject<EventObject>();
            eventObject->type = eventType;
            eventObject->armature = _armature;
            eventObject->animationState = this;
            _armature->_dragonBones->bufferEvent(eventObject);
        }
    }

    if (passedTime < 0.0f)
    {
        passedTime = -passedTime;
    }

    _fadeTime += passedTime;

    if (_fadeTime >= fadeTotalTime)
    {
        _subFadeState = 1;
        _fadeProgress = isFadeOut ? 0.0f : 1.0f;
    }
    else if (_fadeTime > 0.0f)
    {
        _fadeProgress = isFadeOut ? (1.0f - _fadeTime / fadeTotalTime) : (_fadeTime / fadeTotalTime);
    }
    else
    {
        _fadeProgress = isFadeOut ? 1.0f : 0.0f;
    }

    if (_subFadeState > 0)
    {
        if (!isFadeOut)
        {
            _playheadState |= 1; // x1
            _fadeState = 0;
        }

        const auto eventType = isFadeOut ? EventObject::FADE_OUT_COMPLETE : EventObject::FADE_IN_COMPLETE;
        if (_armature->getProxy()->hasDBEventListener(eventType))
        {
            const auto eventObject = BaseObject::borrowObject<EventObject>();
            eventObject->type = eventType;
            eventObject->armature = _armature;
            eventObject->animationState = this;
            _armature->_dragonBones->bufferEvent(eventObject);
        }
    }
}

void AnimationState::init(Armature* parmature, AnimationData* panimationData, AnimationConfig* animationConfig)
{
    if (_armature != nullptr) {
        return;
    }

    _armature = parmature;
    _animationData = panimationData;
    //
    resetToPose = animationConfig->resetToPose;
    additiveBlending = animationConfig->additiveBlending;
    displayControl = animationConfig->displayControl;
    actionEnabled = animationConfig->actionEnabled;
    layer = animationConfig->layer;
    playTimes = animationConfig->playTimes;
    timeScale = animationConfig->timeScale;
    fadeTotalTime = animationConfig->fadeInTime;
    autoFadeOutTime = animationConfig->autoFadeOutTime;
    weight = animationConfig->weight;
    name = !animationConfig->name.empty() ? animationConfig->name : animationConfig->animation;
    group = animationConfig->group;

    if (animationConfig->pauseFadeIn) 
    {
        _playheadState = 2; // 10
    }
    else 
    {
        _playheadState = 3; // 11
    }

    if (animationConfig->duration < 0.0f) 
    {
        _position = 0.0f;
        _duration = _animationData->duration;

        if (animationConfig->position != 0.0f) 
        {
            if (timeScale >= 0.0f) 
            {
                _time = animationConfig->position;
            }
            else 
            {
                _time = animationConfig->position - _duration;
            }
        }
        else 
        {
            _time = 0.0f;
        }
    }
    else 
    {
        _position = animationConfig->position;
        _duration = animationConfig->duration;
        _time = 0.0f;
    }

    if (timeScale < 0.0f && _time == 0.0f) 
    {
        _time = -0.000001f; // Can not cross last frame event.
    }

    if (fadeTotalTime <= 0.0f)
    {
        _fadeProgress = 0.999999f;
    }

    if (!animationConfig->boneMask.empty())
    {
        _boneMask.resize(animationConfig->boneMask.size());
        for (std::size_t i = 0, l = _boneMask.size(); i < l; ++i) {
            _boneMask[i] = animationConfig->boneMask[i];
        }
    }

    _actionTimeline = BaseObject::borrowObject<ActionTimelineState>();
    _actionTimeline->init(_armature, this, _animationData->actionTimeline); //
    _actionTimeline->currentTime = _time;
    if (_actionTimeline->currentTime < 0.0f)
    {
        _actionTimeline->currentTime = _duration - _actionTimeline->currentTime;
    }

    if (_animationData->zOrderTimeline != nullptr)
    {
        _zOrderTimeline = BaseObject::borrowObject<ZOrderTimelineState>();
        _zOrderTimeline->init(_armature, this, _animationData->zOrderTimeline);
    }
}

void AnimationState::_advancePlayhead(float passedTime)
{
    _framePassedTime = passedTime;

    if (_syncLeader != nullptr) // The playhead follows the leader's mapped phase instead of its own timeScale.
    {
        _applySynchronizedTime();
    }
    else if (_playheadState == 3) // 11
    {
        if (timeScale != 1.0f)
        {
            passedTime *= timeScale;
        }

        _time += passedTime;
    }
}

void AnimationState::_advanceTimelines(float cacheFrameRate)
{
    const auto passedTime = _framePassedTime;

    // Update fade time.
    if (_fadeState != 0 || _subFadeState != 0)
    {
        _advanceFadeTime(passedTime);
    }

    if (_timelineDirty != 0) 
    {
        if (_timelineDirty == 2) 
        {
            _updateTimelines();
        }

        _timelineDirty = 0;
        _updateBoneAndSlotTimelines();
    }

    if (weight == 0.0f)
    {
        _syncCrossingPending = false; // No timeline sampling this frame; the crossing would be stale later.
        return;
    }

    const auto isCacheEnabled = _fadeState == 0 && cacheFrameRate > 0.0f;
    auto isUpdateTimeline = true;
    auto isUpdateBoneTimeline = true;
    auto time = _time;
    _weightResult = weight * _fadeProgress;

    if (_syncLeader != nullptr && _syncCrossingPending) // Replay the crossing captured in the playhead pass.
    {
        _actionTimeline->dispatchCrossedEvents(_syncOldTime, _syncNewTime, _syncDirection);
        _syncCrossingPending = false;
    }

    if (_actionTimeline->playState <= 0)
    {
        _actionTimeline->update(time); // Update main timeline.
    }

    if (isCacheEnabled) // Cache time internval.
    {
        const auto internval = cacheFrameRate * 2.0f;
        _actionTimeline->currentTime = (unsigned)(_actionTimeline->currentTime * internval) / internval;
    }

    if (_zOrderTimeline != nullptr && _zOrderTimeline->playState <= 0) // Update zOrder timeline.
    {
        _zOrderTimeline->update(time);
    }

    if (isCacheEnabled) // Update cache.
    {
        const auto cacheFrameIndex = (unsigned)(_actionTimeline->currentTime * cacheFrameRate); // uint
        if ((unsigned)_armature->_cacheFrameIndex == cacheFrameIndex) // Same cache.
        { 
            isUpdateTimeline = false;
            isUpdateBoneTimeline = false;
        }
        else 
        {
            _armature->_cacheFrameIndex = cacheFrameIndex;
            if (_animationData->cachedFrames[cacheFrameIndex]) // Cached.
            { 
                isUpdateBoneTimeline = false;
            }
            else // Cache.
            { 
                _animationData->cachedFrames[cacheFrameIndex] = true;
            }
        }
    }

    if (isUpdateTimeline) 
    {
        if (isUpdateBoneTimeline) // Update bone timelines.
        {
            for (std::size_t i = 0, l = _boneTimelines.size(); i < l; ++i) 
            {
                const auto timeline = _boneTimelines[i];

                if (timeline->playState <= 0) 
                {
                    timeline->update(time);
                }

                if (i == l - 1 || timeline->bone != _boneTimelines[i + 1]->bone) 
                {
                    const auto state = timeline->bone->_blendState.update(_weightResult, layer);
                    if (state != 0) 
                    {
                        timeline->blend(state);
                    }
                }
            }
        }

        if (displayControl) 
        {
            for (std::size_t i = 0, l = _slotTimelines.size(); i < l; ++i)
            {
                const auto timeline = _slotTimelines[i];
                const auto& displayController = timeline->slot->displayController;

                if (
                    displayController.empty() ||
                    displayController == name ||
                    displayController == group
                )
                {
                    if (timeline->playState <= 0)
                    {
                        timeline->update(time);
                    }
                }
            }
        }

        for (std::size_t i = 0, l = _constraintTimelines.size(); i < l; ++i)
        {
            const auto timeline = _constraintTimelines[i];
            if (timeline->playState <= 0) 
            {
                timeline->update(time);
            }
        }
    }

    if (_fadeState == 0) 
    {
        if (_subFadeState > 0) 
        {
            _subFadeState = 0;

            if (!_poseTimelines.empty()) 
            {
                for (const auto& pair : _poseTimelines) 
                {
                    const auto timeline = pair.first;
                    if (pair.second == BaseTimelineType::Bone)
                    { 
                        _boneTimelines.erase(std::find(_boneTimelines.begin(), _boneTimelines.end(), timeline));
                    }
                    else if (pair.second == BaseTimelineType::Slot)
                    {
                        _slotTimelines.erase(std::find(_slotTimelines.begin(), _slotTimelines.end(), timeline));
                    }
                    else if (pair.second == BaseTimelineType::Constraint)
                    {
                        _constraintTimelines.erase(std::find(_constraintTimelines.begin(), _constraintTimelines.end(), timeline));
                    }

                    timeline->returnToPool();
                }

                _poseTimelines.clear();
            }
        }

        if (_actionTimeline->playState > 0)
        {
            if (autoFadeOutTime >= 0.0f) // Auto fade out.
            {
                fadeOut(autoFadeOutTime);
            }
        }
    }
}

void AnimationState::advanceTime(float passedTime, float cacheFrameRate)
{
    _advancePlayhead(passedTime);
    _advanceTimelines(cacheFrameRate);
}

void AnimationState::play()
{
    _playheadState = 3; // 11
}

void AnimationState::stop()
{
    _playheadState &= 1; // 0x
}

void AnimationState::fadeOut(float fadeOutTime, bool pausePlayhead)
{
    if (fadeOutTime < 0.0f)
    {
        fadeOutTime = 0.0f;
    }

    if (pausePlayhead) 
    {
        _playheadState &= 2; // x0
    }

    if (_fadeState > 0)
    {
        if (fadeOutTime > fadeTotalTime - _fadeTime) // If the animation is already in fade out, the new fade out will be ignored.
        {
            return;
        }
    }
    else
    {
        _fadeState = 1;
        _subFadeState = -1;

        if (fadeOutTime <= 0.0f || _fadeProgress <= 0.0f)
        {
            _fadeProgress = 0.000001f; // Modify fade progress to different value.
        }

        for (const auto timeline : _boneTimelines)
        {
            timeline->fadeOut();
        }

        for (const auto timeline : _slotTimelines)
        {
            timeline->fadeOut();
        }
    }

    displayControl = false; //
    fadeTotalTime = _fadeProgress > 0.000001f ? fadeOutTime / _fadeProgress : 0.0f;
    _fadeTime = fadeTotalTime * (1.0f - _fadeProgress);
}

bool AnimationState::containsBoneMask(const std::string& boneName) const
{
    return _boneMask.empty() || std::find(_boneMask.cbegin(), _boneMask.cend(), boneName) != _boneMask.cend();
}

void AnimationState::addBoneMask(const std::string& boneName, bool recursive)
{
    const auto currentBone = _armature->getBone(boneName);
    if (currentBone == nullptr)
    {
        return;
    }

    if (std::find(_boneMask.cbegin(), _boneMask.cend(), boneName) == _boneMask.cend())
    {
        _boneMask.push_back(boneName);
    }

    if (recursive) // Add recursive mixing.
    {
        for (const auto bone : _armature->getBones())
        {
            if (std::find(_boneMask.cbegin(), _boneMask.cend(), bone->getName()) == _boneMask.cend() && currentBone->contains(bone))
            {
                _boneMask.push_back(bone->getName());
            }
        }
    }

    _timelineDirty = 1;
}

void AnimationState::removeBoneMask(const std::string& boneName, bool recursive)
{
    {
        auto iterator = std::find(_boneMask.begin(), _boneMask.end(), boneName);
        if (iterator != _boneMask.cend()) // Remove mixing.
        {
            _boneMask.erase(iterator);
        }
    }

    if (recursive)
    {
        const auto currentBone = _armature->getBone(boneName);
        if (currentBone != nullptr)
        {
            const auto& bones = _armature->getBones();
            if (!_boneMask.empty()) // Remove recursive mixing.
            {
                for (const auto bone : bones)
                {
                    auto iterator = std::find(_boneMask.begin(), _boneMask.end(), bone->getName());
                    if (iterator != _boneMask.end() && currentBone->contains(bone))
                    {
                        _boneMask.erase(iterator);
                    }
                }
            }
            else // Add unrecursive mixing.
            {
                for (const auto bone : bones)
                {
                    if (bone == currentBone) 
                    {
                        continue;
                    }

                    if (!currentBone->contains(bone)) 
                    {
                        _boneMask.push_back(bone->getName());
                    }
                }
            }
        }
    }

    _timelineDirty = 1;
}

void AnimationState::removeAllBoneMask()
{
    _boneMask.clear();
    _timelineDirty = 1;
}

bool AnimationState::isPlaying() const
{
    return (_playheadState & 2) != 0 && _actionTimeline->playState <= 0;
}

bool AnimationState::isCompleted() const
{
    return _actionTimeline->playState > 0;
}

unsigned AnimationState::getCurrentPlayTimes() const
{
    return _actionTimeline->currentPlayTimes;
}

float AnimationState::getCurrentTime() const
{
    return _actionTimeline->currentTime;
}

void AnimationState::setCurrentTime(float value)
{
    const auto currentPlayTimes = _actionTimeline->currentPlayTimes - (_actionTimeline->playState > 0 ? 1 : 0);
    if (value < 0.0f || _duration < value) 
    {
        value = fmod(value, _duration) + currentPlayTimes * _duration;
        if (value < 0.0f) 
        {
            value += _duration;
        }
    }

    if (playTimes > 0 && (unsigned)currentPlayTimes == playTimes - 1 && value == _duration) 
    {
        value = _duration - 0.000001f;
    }

    if (_time == value) 
    {
        return;
    }

    _time = value;
    _actionTimeline->setCurrentTime(_time);

    if (_zOrderTimeline != nullptr) 
    {
        _zOrderTimeline->playState = -1;
    }

    for (const auto timeline : _boneTimelines)
    {
        timeline->playState = -1;
    }

    for (const auto timeline : _slotTimelines)
    {
        timeline->playState = -1;
    }
}

bool AnimationState::_validateSyncMarkers(const std::vector<std::pair<float, float>>& markers, float leaderDuration) const
{
    if (markers.size() < 2 || _duration <= 0.0f || leaderDuration <= 0.0f)
    {
        return false;
    }

    for (const auto& marker : markers) // Every value must be a finite number.
    {
        if (!std::isfinite(marker.first) || !std::isfinite(marker.second))
        {
            return false;
        }
    }

    if (markers.front().first != 0.0f || markers.front().second != 0.0f)
    {
        return false;
    }

    if (markers.back().first != leaderDuration || markers.back().second != _duration)
    {
        return false;
    }

    for (std::size_t i = 1; i < markers.size(); ++i) // Both columns must be strictly increasing.
    {
        if (!(markers[i].first > markers[i - 1].first) || !(markers[i].second > markers[i - 1].second))
        {
            return false;
        }
    }

    return true;
}

float AnimationState::_mapSyncTime(float leaderTime) const
{
    const auto leaderDuration = _syncLeader->_duration;
    const auto followerDuration = _duration;

    auto cycle = std::floor(leaderTime / leaderDuration);
    auto localLeaderTime = leaderTime - cycle * leaderDuration;
    if (localLeaderTime >= leaderDuration) // Keep the floating point remainder inside [0, duration).
    {
        localLeaderTime -= leaderDuration;
        cycle += 1.0f;
    }
    else if (localLeaderTime < 0.0f)
    {
        localLeaderTime += leaderDuration;
        cycle -= 1.0f;
    }

    std::size_t segment = 0;
    while (segment + 2 < _syncMarkers.size() && _syncMarkers[segment + 1].first <= localLeaderTime)
    {
        ++segment;
    }

    const auto& from = _syncMarkers[segment];
    const auto& to = _syncMarkers[segment + 1];
    const auto leaderSpan = to.first - from.first;
    const auto progress = leaderSpan > 0.0f ? (localLeaderTime - from.first) / leaderSpan : 0.0f;
    const auto localFollowerTime = from.second + (to.second - from.second) * progress;

    return cycle * followerDuration + localFollowerTime;
}

void AnimationState::_applySynchronizedTime()
{
    const auto leader = _syncLeader;
    if (leader == nullptr)
    {
        return;
    }

    const auto leaderDirection = leader->_getPlayDirection();
    if (leaderDirection != 0)
    {
        _syncDirection = leaderDirection;
    }

    const auto newTime = _mapSyncTime(leader->_time);
    const auto oldTime = _time;

    if (newTime == oldTime)
    {
        _time = newTime;
        _syncCrossingPending = false;
        return;
    }

    _time = newTime;
    // Reposition every timeline through the regular pipeline, but swallow the frame events
    // generated by the jump; the strictly crossed events are re-derived below.
    _syncSuppressFrameEvents = true;
    _actionTimeline->setCurrentTime(newTime);

    if (_zOrderTimeline != nullptr)
    {
        _zOrderTimeline->playState = -1;
    }

    for (const auto timeline : _boneTimelines)
    {
        timeline->playState = -1;
    }

    for (const auto timeline : _slotTimelines)
    {
        timeline->playState = -1;
    }

    for (const auto timeline : _constraintTimelines)
    {
        timeline->playState = -1;
    }

    _syncSuppressFrameEvents = false;
    if (newTime != oldTime) // The crossing itself is replayed in the timeline pass.
    {
        _syncCrossingPending = true;
        _syncOldTime = oldTime;
        _syncNewTime = newTime;
    }
}

void AnimationState::_clearSyncBinding()
{
    _syncLeader = nullptr;
    _syncMarkers.clear();
    _syncSuppressFrameEvents = false;
    _syncCrossingPending = false;
    // _time is kept: playback resumes from the last mapped time.
}

void AnimationState::_detachSyncOnRemove()
{
    if (_syncLeader != nullptr)
    {
        auto& followers = _syncLeader->_syncFollowers;
        followers.erase(std::remove(followers.begin(), followers.end(), this), followers.end());
    }

    for (const auto follower : _syncFollowers) // Followers become independent, keeping their last mapped time.
    {
        if (follower->_syncLeader == this)
        {
            follower->_clearSyncBinding();
        }
    }

    _syncFollowers.clear();
    _clearSyncBinding();
}

bool AnimationState::synchronizeTo(AnimationState* leader, const std::vector<std::pair<float, float>>& markers)
{
    if (leader == nullptr || leader == this)
    {
        return false;
    }

    if (_armature == nullptr || leader->_armature != _armature || leader->_animationData == nullptr)
    {
        return false;
    }

    const auto& states = _armature->getAnimation()->getStates();
    if (std::find(states.cbegin(), states.cend(), this) == states.cend() ||
        std::find(states.cbegin(), states.cend(), leader) == states.cend())
    {
        return false; // Both states must be active in this armature.
    }

    const auto isFullLoop = [](const AnimationState* state) {
        return state->playTimes == 0 &&
               state->_position == 0.0f &&
               state->_duration > 0.0f &&
               state->_duration == state->_animationData->duration;
    };

    if (!isFullLoop(this) || !isFullLoop(leader)) // Only full, infinitely looping states are supported.
    {
        return false;
    }

    for (auto* ancestor = leader; ancestor != nullptr; ancestor = ancestor->_syncLeader)
    {
        if (ancestor == this)
        {
            return false; // Would create a cycle.
        }
    }

    if (!_validateSyncMarkers(markers, leader->_duration))
    {
        return false; // The existing binding (if any) is untouched until every check passes.
    }

    if (_syncLeader != nullptr && _syncLeader != leader)
    {
        auto& previousFollowers = _syncLeader->_syncFollowers;
        previousFollowers.erase(std::remove(previousFollowers.begin(), previousFollowers.end(), this), previousFollowers.end());
    }

    _syncLeader = leader;
    if (std::find(leader->_syncFollowers.cbegin(), leader->_syncFollowers.cend(), this) == leader->_syncFollowers.cend())
    {
        leader->_syncFollowers.push_back(this);
    }

    _syncMarkers = markers;
    _syncSuppressFrameEvents = false;
    _syncCrossingPending = false;
    _syncDirection = leader->_getPlayDirection() == 0 ? 1 : leader->_getPlayDirection();

    // Locate the current phase. Binding itself dispatches no events.
    const auto mappedTime = _mapSyncTime(leader->_time);
    _time = mappedTime;
    _actionTimeline->setCurrentTime(mappedTime);

    if (_zOrderTimeline != nullptr)
    {
        _zOrderTimeline->playState = -1;
    }

    for (const auto timeline : _boneTimelines)
    {
        timeline->playState = -1;
    }

    for (const auto timeline : _slotTimelines)
    {
        timeline->playState = -1;
    }

    for (const auto timeline : _constraintTimelines)
    {
        timeline->playState = -1;
    }

    return true;
}

void AnimationState::unsynchronize()
{
    if (_syncLeader == nullptr)
    {
        return;
    }

    auto& followers = _syncLeader->_syncFollowers;
    followers.erase(std::remove(followers.begin(), followers.end(), this), followers.end());
    _clearSyncBinding();
}

void BonePose::_onClear()
{
    current.identity();
    delta.identity();
    result.identity();
}

int BlendState::update(float weight, int p_layer)
{
    if (dirty)
    {
        if (leftWeight > 0.0f)
        {
            if (layer != p_layer) 
            {
                if (layerWeight >= leftWeight) 
                {
                    leftWeight = 0.0f;

                    return 0;
                }
                else 
                {
                    layer = p_layer;
                    leftWeight -= layerWeight;
                    layerWeight = 0.0f;
                }
            }
        }
        else 
        {
            return 0;
        }

        weight *= leftWeight;
        layerWeight += weight;
        blendWeight = weight;

        return 2;
    }

    dirty = true;
    layer = p_layer;
    layerWeight = weight;
    leftWeight = 1.0f;
    blendWeight = weight;

    return 1;
}

void BlendState::clear()
{
    dirty = false;
    layer = 0;
    leftWeight = 0.0f;
    layerWeight = 0.0f;
    blendWeight = 0.0f;
}


DRAGONBONES_NAMESPACE_END

