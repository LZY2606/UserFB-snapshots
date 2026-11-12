/**
 * Phase synchronization regression tests for AnimationState::synchronizeTo / unsynchronize.
 *
 * Minimal call example:
 * @code
 *     auto* walk = armature->getAnimation()->play("walk", 0); // leader, infinite loop
 *     auto* run  = armature->getAnimation()->fadeIn("run", 0.0f, 0); // follower, infinite loop
 *     const std::vector<std::pair<float, float>> markers = {
 *         {0.0f, 0.0f}, {0.25f, 0.125f}, {0.5f, 0.4f}, {1.0f, 0.5f}
 *     };
 *     if (run->synchronizeTo(walk, markers))
 *     {
 *         armature->advanceTime(0.1f); // run keeps following walk's phase through the markers
 *         run->unsynchronize();        // run resumes its own timeScale from the last mapped time
 *     }
 * @endcode
 */
#if __has_include("checks/test-harness.h")
#include "checks/test-harness.h"
#include "checks/headless.h"
#else
#include "../checks/test-harness.h"
#include "../checks/headless.h"
#endif

#include <cmath>
#include <limits>

using namespace dragonBones;

namespace
{
    // Headless proxy that records every dispatched DragonBones event.
    struct RecordingProxy : HeadlessProxy
    {
        struct Event
        {
            std::string type;
            std::string name;
            float time;
            std::string state;
        };

        std::vector<Event> events;
        std::vector<std::string> listened;

        bool hasDBEventListener(const std::string& type) const override
        {
            return std::find(listened.cbegin(), listened.cend(), type) != listened.cend();
        }

        void dispatchDBEvent(const std::string& type, EventObject* value) override
        {
            events.push_back({
                type,
                value->name,
                value->time,
                value->animationState != nullptr ? value->animationState->name : std::string()
            });
        }
    };

    // Two short animations (walk 1.0s, run 0.5s) on a parent/child bone pair, each with frame events.
    const char* FIXTURE_JSON =
        "{"
        "  \"version\": \"5.5\", \"compatibleVersion\": \"5.5\", \"frameRate\": 24, \"name\": \"data\","
        "  \"armature\": [{"
        "    \"name\": \"rig\","
        "    \"bone\": [{\"name\": \"root\"}, {\"name\": \"tip\", \"parent\": \"root\", \"transform\": {\"x\": 3}}],"
        "    \"animation\": ["
        "      {"
        "        \"name\": \"walk\", \"duration\": 24, \"playTimes\": 0,"
        "        \"frame\": ["
        "          {\"duration\": 6},"
        "          {\"duration\": 12, \"event\": \"w1\"},"
        "          {\"duration\": 6, \"event\": \"w2\"}"
        "        ],"
        "        \"bone\": [{"
        "          \"name\": \"root\","
        "          \"translateFrame\": ["
        "            {\"duration\": 12, \"x\": 0, \"tweenEasing\": 0},"
        "            {\"duration\": 12, \"x\": 8, \"tweenEasing\": 0}"
        "          ]"
        "        }]"
        "      },"
        "      {"
        "        \"name\": \"run\", \"duration\": 12, \"playTimes\": 0,"
        "        \"frame\": ["
        "          {\"duration\": 3},"
        "          {\"duration\": 6, \"event\": \"r1\"},"
        "          {\"duration\": 3, \"event\": \"r2\"}"
        "        ],"
        "        \"bone\": [{"
        "          \"name\": \"root\","
        "          \"translateFrame\": ["
        "            {\"duration\": 6, \"x\": 0, \"tweenEasing\": 0},"
        "            {\"duration\": 6, \"x\": 4, \"tweenEasing\": 0}"
        "          ]"
        "        }]"
        "      }"
        "    ]"
        "  }]"
        "}";

    struct Fixture
    {
        RecordingProxy proxy;
        DragonBones runtime;
        DragonBonesData* data = nullptr;
        ArmatureData* armatureData = nullptr;
        Armature* armature = nullptr;
        bool released = false;

        Fixture() :
            runtime(&proxy)
        {
            proxy.listened = { EventObject::FRAME_EVENT, EventObject::LOOP_COMPLETE, EventObject::START };
            JSONDataParser parser;
            data = parser.parseDragonBonesData(FIXTURE_JSON, 1.0f);
            armatureData = data->getArmature("rig");
            armature = BaseObject::borrowObject<Armature>();
            armature->init(armatureData, &proxy, nullptr, &runtime);
            for (auto* boneData : armatureData->sortedBones)
            {
                auto* bone = BaseObject::borrowObject<Bone>();
                bone->init(boneData, armature);
            }

            armature->getAnimation()->setAnimations(armatureData->animations);
        }

        ~Fixture()
        {
            if (!released)
            {
                armature->returnToPool();
                data->returnToPool();
                BaseObject::clearPool();
            }
        }

        // Advances the armature and flushes the buffered events through the recording proxy.
        void step(float passedTime)
        {
            armature->advanceTime(passedTime);
            runtime.advanceTime(0.0f);
        }

        // Plays the run state on a higher layer so it owns the full bone weight by itself.
        AnimationState* playFollower(const std::string& name = "run", unsigned layer = 1)
        {
            auto* state = animation()->fadeIn(name, 0.0f, 0, (int)layer, "", AnimationFadeOutMode::None);
            state->resetToPose = false;
            return state;
        }

        void release()
        {
            armature->returnToPool();
            data->returnToPool();
            BaseObject::clearPool();
            released = true;
        }

        Animation* animation()
        {
            return armature->getAnimation();
        }

        std::vector<std::string> eventsOf(const std::string& stateName) const
        {
            std::vector<std::string> result;
            for (const auto& event : proxy.events)
            {
                if (event.state != stateName)
                {
                    continue;
                }

                if (event.type == EventObject::LOOP_COMPLETE)
                {
                    result.push_back("loop");
                }
                else if (event.type == EventObject::FRAME_EVENT)
                {
                    result.push_back(event.name);
                }
            }

            return result;
        }

        float rootTx() const
        {
            return armature->getBone("root")->globalTransformMatrix.tx;
        }

        float tipTx() const
        {
            return armature->getBone("tip")->globalTransformMatrix.tx;
        }
    };

    // Non-uniform markers: walk 1.0s -> run 0.5s. walk 0.25 maps to run 0.125 (the r1 frame).
    std::vector<std::pair<float, float>> makeMarkers()
    {
        return { {0.0f, 0.0f}, {0.25f, 0.125f}, {0.5f, 0.4f}, {1.0f, 0.5f} };
    }

    // Reference implementation of the required piecewise linear mapping on expanded times.
    float referenceMap(float leaderTime, const std::vector<std::pair<float, float>>& markers, float leaderDuration, float followerDuration)
    {
        const auto cycle = std::floor(leaderTime / leaderDuration);
        auto local = leaderTime - cycle * leaderDuration;
        if (local >= leaderDuration)
        {
            local -= leaderDuration;
        }

        std::size_t segment = 0;
        while (segment + 2 < markers.size() && markers[segment + 1].first <= local)
        {
            ++segment;
        }

        const auto& from = markers[segment];
        const auto& to = markers[segment + 1];
        const auto progress = (local - from.first) / (to.first - from.first);
        return cycle * followerDuration + from.second + (to.second - from.second) * progress;
    }

    bool close(float a, float b, float epsilon = 1e-5f)
    {
        return std::fabs(a - b) <= epsilon;
    }
}

// 1. Pose follows the mapped phase, through the existing bone blending and layer rules.
DB_CASE(phase_pose)
{
    const auto markers = makeMarkers();

    Fixture fixture;
    auto* const walk = fixture.animation()->play("walk", 0);
    auto* const run = fixture.playFollower();

    fixture.step(0.25f); // Walk reaches 0.25s before the binding is created.
    fixture.proxy.events.clear(); // Discard everything the unbound follower played before binding.
    DB_CHECK(run->synchronizeTo(walk, markers));

    // Binding only locates the phase: a zero-length step dispatches no backfilled events.
    DB_CHECK(fixture.eventsOf("run").empty());
    fixture.step(0.0f);
    DB_CHECK(fixture.eventsOf("run").empty());
    // walk 0.25 -> run 0.125; run root.x tweens 0 -> 4 across the first 0.25s, so x = 2.
    DB_CHECK(close(run->getCurrentTime(), 0.125f, 1e-6f));
    DB_CHECK(close(fixture.rootTx(), 2.0f, 1e-4f));
    DB_CHECK(close(fixture.tipTx(), 5.0f, 1e-4f)); // Child bone inherits the parent translation.

    fixture.step(0.25f); // Walk reaches 0.5, exactly on a marker: run maps to 0.4.
    DB_CHECK(close(run->getCurrentTime(), 0.4f, 1e-6f));
    DB_CHECK(close(fixture.rootTx(), 1.6f, 1e-4f)); // run 0.4 is in the wrap tween back to 0.
    DB_CHECK(close(fixture.tipTx(), 4.6f, 1e-4f));

    // The mapping is not a fixed time scale: the three segments have slopes 0.5, 1.1 and 0.2.
    DB_CHECK(close(referenceMap(0.125f, markers, 1.0f, 0.5f), 0.0625f, 1e-6f));
    DB_CHECK(close(referenceMap(0.375f, markers, 1.0f, 0.5f), 0.2625f, 1e-6f));
    DB_CHECK(close(referenceMap(0.75f, markers, 1.0f, 0.5f), 0.45f, 1e-6f));

    // Same-layer states still go through the regular bone blending (poses are added by BlendState).
    Fixture blendFixture;
    auto* const blendWalk = blendFixture.animation()->play("walk", 0);
    auto* const blendRun = blendFixture.animation()->fadeIn("run", 0.0f, 0, 0, "", AnimationFadeOutMode::None);
    blendRun->resetToPose = false;
    DB_CHECK(blendRun->synchronizeTo(blendWalk, markers));
    blendFixture.step(0.25f);
    const auto walkPoseX = 8.0f * (0.25f / 0.5f);  // walk root.x at 0.25s
    const auto runPoseX = 4.0f * (0.125f / 0.25f); // run root.x at mapped 0.125s
    DB_CHECK(close(blendFixture.rootTx(), walkPoseX + runPoseX, 1e-4f)); // 4 + 2 = 6

    // A follower created earlier and living on a higher layer still uses the leader's phase.
    Fixture layerFixture;
    auto* const highRun = layerFixture.animation()->fadeIn("run", 0.0f, 0, 2, "", AnimationFadeOutMode::None);
    auto* const lowWalk = layerFixture.animation()->fadeIn("walk", 0.0f, 0, 0, "", AnimationFadeOutMode::None);
    DB_CHECK(highRun->synchronizeTo(lowWalk, markers));
    layerFixture.step(0.25f);
    DB_CHECK(close(highRun->getCurrentTime(), 0.125f, 1e-6f));
    DB_CHECK(close(layerFixture.rootTx(), runPoseX, 1e-4f)); // The higher layer owns the full weight.

    // A follower inserted before its leader in the state list still reads the leader's fresh time.
    Fixture earlierFixture;
    auto* const earlyRun = earlierFixture.animation()->fadeIn("run", 0.0f, 0, 0, "", AnimationFadeOutMode::None);
    earlyRun->resetToPose = false;
    auto* const lateWalk = earlierFixture.animation()->fadeIn("walk", 0.0f, 0, 0, "", AnimationFadeOutMode::None);
    DB_CHECK(earlierFixture.animation()->getStates().front() == earlyRun); // Follower sits earlier in the list.
    DB_CHECK(earlyRun->synchronizeTo(lateWalk, markers));
    earlierFixture.step(0.25f);
    DB_CHECK(close(lateWalk->getCurrentTime(), 0.25f, 1e-6f));
    DB_CHECK(close(earlyRun->getCurrentTime(), 0.125f, 1e-6f));
    DB_CHECK(close(earlierFixture.rootTx(), walkPoseX + runPoseX, 1e-4f)); // Same-layer additive blend preserved.

    // A faster leader keeps the mapping correct, and the follower's fade still runs on elapsed time.
    Fixture speedFixture;
    auto* const speedWalk = speedFixture.animation()->play("walk", 0);
    auto* const speedRun = speedFixture.animation()->fadeIn("run", 0.3f, 0, 1, "", AnimationFadeOutMode::None);
    speedRun->resetToPose = false;
    speedRun->timeScale = 9.0f; // Ignored for sampling and for the fade clock.
    DB_CHECK(speedRun->synchronizeTo(speedWalk, markers));
    speedWalk->timeScale = 2.0f;
    speedFixture.step(0.25f); // The leader advances 0.5s of animation time.
    DB_CHECK(close(speedRun->getCurrentTime(), 0.4f, 1e-6f));
    DB_CHECK(speedRun->isFadeIn()); // Only 0.25s of the 0.3s fade has elapsed.
    speedFixture.step(0.1f);
    DB_CHECK(speedRun->isFadeComplete()); // The fade completed from elapsed time, not the follower's timeScale.
    DB_CHECK(close(speedRun->getCurrentTime(), referenceMap(0.7f, markers, 1.0f, 0.5f), 1e-5f));
}

// 2. Frame events and loops: one big jump equals many small steps, with half-open crossing intervals.
DB_CASE(crossings_and_loops)
{
    const auto markers = makeMarkers();

    Fixture bigStep;
    auto* bigWalk = bigStep.animation()->play("walk", 0);
    auto* bigRun = bigStep.playFollower();
    DB_CHECK(bigRun->synchronizeTo(bigWalk, markers));
    bigStep.step(2.3f); // Crosses several run loops in a single advance.

    Fixture smallSteps;
    auto* smallWalk = smallSteps.animation()->play("walk", 0);
    auto* smallRun = smallSteps.playFollower();
    DB_CHECK(smallRun->synchronizeTo(smallWalk, markers));
    for (int i = 0; i < 23; ++i)
    {
        smallSteps.step(0.1f);
    }

    // walk 2.3 -> run expanded 1.18: r1,r2,r1,r2,r1 with loop completes at 0.5 and 1.0.
    const std::vector<std::string> expected = { "r1", "r2", "loop", "r1", "r2", "loop", "r1" };
    DB_CHECK(bigStep.eventsOf("run") == expected);
    DB_CHECK(smallSteps.eventsOf("run") == expected);

    // Final bone world matrices must match regardless of how the advance was split.
    DB_CHECK(close(bigStep.rootTx(), smallSteps.rootTx(), 1e-4f));
    DB_CHECK(close(bigStep.tipTx(), smallSteps.tipTx(), 1e-4f));
    DB_CHECK(close(bigRun->getCurrentTime(), smallRun->getCurrentTime(), 1e-5f));

    // Landing exactly on a frame event fires it exactly once; a zero-length step neither
    // duplicates the event nor moves the pose.
    Fixture boundary;
    auto* boundaryWalk = boundary.animation()->play("walk", 0);
    auto* boundaryRun = boundary.playFollower();
    DB_CHECK(boundaryRun->synchronizeTo(boundaryWalk, markers));
    boundary.step(0.25f); // run lands exactly on its r1 event at 0.125s.
    DB_CHECK(boundary.eventsOf("run") == std::vector<std::string>({ "r1" }));
    const auto poseAtEvent = boundary.rootTx();
    boundary.step(0.0f);
    DB_CHECK(boundary.eventsOf("run") == std::vector<std::string>({ "r1" }));
    DB_CHECK(close(boundary.rootTx(), poseAtEvent, 1e-6f));

    boundary.step(0.75f); // Walk reaches exactly 1.0: run lands exactly on the 0.5 boundary.
    const auto boundaryEvents = boundary.eventsOf("run");
    DB_CHECK(std::count(boundaryEvents.cbegin(), boundaryEvents.cend(), "loop") == 1);
    DB_CHECK(boundaryEvents.back() == "loop");
    DB_CHECK(close(boundaryRun->getCurrentTime(), 0.0f, 1e-6f)); // Wrapped cleanly, no duplicated phase.
}

// 3. Reverse playback, negative expanded times and pausing keep the follower deterministic.
DB_CASE(reverse_and_pause)
{
    const auto markers = makeMarkers();

    Fixture fixture;
    auto* walk = fixture.animation()->play("walk", 0);
    auto* run = fixture.playFollower();
    run->timeScale = 5.0f; // The follower's own timeScale must not affect sampling while bound.
    DB_CHECK(run->synchronizeTo(walk, markers));
    fixture.step(0.5f); // walk 0.5 -> run 0.4
    DB_CHECK(close(run->getCurrentTime(), 0.4f, 1e-6f));
    fixture.proxy.events.clear();

    walk->timeScale = -1.0f;
    fixture.step(0.25f); // walk back to 0.25 -> run back to 0.125.
    DB_CHECK(close(run->getCurrentTime(), 0.125f, 1e-6f));
    DB_CHECK(close(fixture.rootTx(), 2.0f, 1e-4f));
    // Reverse interval [0.125, 0.4): r2 at 0.375 first, then r1 exactly on the new time 0.125.
    DB_CHECK(fixture.eventsOf("run") == std::vector<std::string>({ "r2", "r1" }));

    // Negative expanded time: walking backwards from the start for 1.3s (leader starts at -1e-6).
    Fixture negative;
    auto* negWalk = negative.animation()->play("walk", 0);
    auto* negRun = negative.playFollower();
    negWalk->timeScale = -1.0f;
    DB_CHECK(negRun->synchronizeTo(negWalk, markers));
    negative.step(1.3f);
    // walk ~-1.3 maps to run expanded ~-0.56, i.e. local phase 0.44; the wrap tween gives root.x 0.96.
    DB_CHECK(close(negRun->getCurrentTime(), 0.44f, 1e-4f));
    DB_CHECK(close(negative.rootTx(), 0.96f, 1e-3f));

    // A large reverse jump and the equivalent small reverse steps produce the same event order.
    Fixture reverseBig;
    auto* rbWalk = reverseBig.animation()->play("walk", 0);
    auto* rbRun = reverseBig.playFollower();
    DB_CHECK(rbRun->synchronizeTo(rbWalk, markers));
    reverseBig.step(2.0f);
    rbWalk->timeScale = -1.0f;
    reverseBig.step(1.7f);

    Fixture reverseSmall;
    auto* rsWalk = reverseSmall.animation()->play("walk", 0);
    auto* rsRun = reverseSmall.playFollower();
    DB_CHECK(rsRun->synchronizeTo(rsWalk, markers));
    reverseSmall.step(2.0f);
    rsWalk->timeScale = -1.0f;
    for (int i = 0; i < 17; ++i)
    {
        reverseSmall.step(0.1f);
    }

    DB_CHECK(reverseBig.eventsOf("run") == reverseSmall.eventsOf("run"));
    DB_CHECK(close(reverseBig.rootTx(), reverseSmall.rootTx(), 1e-4f));
    DB_CHECK(close(rbRun->getCurrentTime(), rsRun->getCurrentTime(), 1e-5f));

    // Pausing the leader freezes the follower; changing Animation.timeScale only rescales both.
    Fixture paused;
    auto* pauseWalk = paused.animation()->play("walk", 0);
    auto* pauseRun = paused.playFollower();
    DB_CHECK(pauseRun->synchronizeTo(pauseWalk, markers));
    paused.step(0.5f);
    pauseWalk->timeScale = 0.0f;
    const auto frozenTime = pauseRun->getCurrentTime();
    const auto frozenPose = paused.rootTx();
    paused.step(0.3f);
    paused.step(0.3f);
    DB_CHECK(close(pauseRun->getCurrentTime(), frozenTime, 1e-6f));
    DB_CHECK(close(paused.rootTx(), frozenPose, 1e-6f));

    pauseWalk->timeScale = 1.0f;
    paused.animation()->timeScale = 0.5f;
    paused.step(0.5f); // Only 0.25s of animation time elapses.
    DB_CHECK(close(pauseWalk->getCurrentTime(), 0.75f, 1e-5f));
    DB_CHECK(close(pauseRun->getCurrentTime(), referenceMap(0.75f, markers, 1.0f, 0.5f), 1e-5f));
}

// 4. Chains, lifecycle teardown, pool reuse and armature release stay safe.
DB_CASE(lifecycle_and_chain)
{
    const auto markers = makeMarkers();

    Fixture fixture;
    auto* walk = fixture.animation()->play("walk", 0);
    auto* walk2Config = fixture.animation()->getAnimationConfig();
    walk2Config->clear();
    walk2Config->animation = "walk";
    walk2Config->name = "walk2";
    walk2Config->playTimes = 0;
    walk2Config->fadeInTime = 0.0f;
    walk2Config->fadeOutMode = AnimationFadeOutMode::None;
    auto* walk2 = fixture.animation()->playConfig(walk2Config);
    auto* run = fixture.playFollower();

    // Chain: run -> walk2 -> walk. The two walk states share the 1.0s duration, so their
    // marker table ends at (1, 1); run keeps its own 0.5s table.
    const std::vector<std::pair<float, float>> walkMarkers = { {0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 1.0f} };
    DB_CHECK(walk2->synchronizeTo(walk, walkMarkers));
    DB_CHECK(run->synchronizeTo(walk2, markers));
    fixture.step(0.25f);
    DB_CHECK(close(walk2->getCurrentTime(), walk->getCurrentTime(), 1e-6f));
    DB_CHECK(close(run->getCurrentTime(), 0.125f, 1e-6f));

    // Self reference and cycles are rejected without disturbing the existing chain.
    DB_CHECK(!run->synchronizeTo(run, markers));
    DB_CHECK(!walk2->synchronizeTo(run, markers));
    DB_CHECK(run->getSyncLeader() == walk2);
    DB_CHECK(walk2->getSyncLeader() == walk);

    // Cross armature binding and null leaders are rejected.
    Fixture other;
    other.animation()->play("walk", 0);
    auto* otherRun = other.animation()->play("run", 0);
    DB_CHECK(!run->synchronizeTo(otherRun, markers));
    DB_CHECK(!otherRun->synchronizeTo(run, markers));
    DB_CHECK(!run->synchronizeTo(nullptr, markers));

    // Fade out and remove the leader: the follower detaches and continues on its own timeScale.
    walk->fadeOut(0.0f, false);
    fixture.step(0.1f); // The fade completes during this frame.
    fixture.step(0.1f); // The finished state is collected at the start of the next frame.
    DB_CHECK(fixture.animation()->getState("walk") == nullptr);
    DB_CHECK(!walk2->isSynchronized());
    const auto resumeTime = walk2->getCurrentTime();
    fixture.step(0.2f);
    DB_CHECK(close(walk2->getCurrentTime(), resumeTime + 0.2f, 1e-5f));

    // reset() must tear every relation down safely.
    run->unsynchronize();
    const std::vector<std::pair<float, float>> runToWalkMarkers = { {0.0f, 0.0f}, {0.25f, 0.5f}, {0.5f, 1.0f} };
    DB_CHECK(walk2->synchronizeTo(run, runToWalkMarkers));
    fixture.animation()->reset();
    DB_CHECK(!run->isSynchronized());
    DB_CHECK(!walk2->isSynchronized());

    // Releasing the armature returns the states to the pool; reused instances must be clean.
    fixture.release();
    auto* reused = BaseObject::borrowObject<AnimationState>();
    DB_CHECK(reused->getSyncLeader() == nullptr);
    DB_CHECK(!reused->isSynchronized());
    DB_CHECK(reused->_syncFollowers.empty());
    reused->returnToPool();

    // A freshly built armature still works after the whole pool was recycled.
    Fixture rebuilt;
    auto* rebuiltWalk = rebuilt.animation()->play("walk", 0);
    auto* rebuiltRun = rebuilt.playFollower();
    DB_CHECK(rebuiltRun->synchronizeTo(rebuiltWalk, markers));
    rebuilt.step(0.25f);
    DB_CHECK(close(rebuiltRun->getCurrentTime(), 0.125f, 1e-6f));
}

// 5. Invalid arguments, unsupported playback configs and legacy playback keep working.
DB_CASE(invalid_and_legacy)
{
    const auto markers = makeMarkers();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();

    Fixture fixture;
    auto* walk = fixture.animation()->play("walk", 0);
    auto* run = fixture.playFollower();
    DB_CHECK(run->synchronizeTo(walk, markers));
    fixture.step(0.25f);
    DB_CHECK(close(run->getCurrentTime(), 0.125f, 1e-6f));

    const auto rejectAndKeep = [&](const std::vector<std::pair<float, float>>& bad) {
        DB_CHECK(!run->synchronizeTo(walk, bad));
        DB_CHECK(run->getSyncLeader() == walk); // Original binding untouched.
        fixture.step(0.1f);
        DB_CHECK(close(run->getCurrentTime(),
                       referenceMap(walk->getCurrentTime(), markers, 1.0f, 0.5f), 1e-5f));
    };

    rejectAndKeep({});
    rejectAndKeep({ {0.0f, 0.0f} });
    rejectAndKeep({ {0.1f, 0.0f}, {1.0f, 0.5f} });                 // First pair is not (0, 0).
    rejectAndKeep({ {0.0f, 0.0f}, {1.0f, 0.4f} });                 // Last pair misses the durations.
    rejectAndKeep({ {0.0f, 0.0f}, {0.5f, 0.4f}, {0.5f, 0.5f} });   // Leader column not strict.
    rejectAndKeep({ {0.0f, 0.0f}, {0.5f, 0.4f}, {1.0f, 0.4f} });   // Follower column not strict.
    rejectAndKeep({ {0.0f, 0.0f}, {0.5f, nan}, {1.0f, 0.5f} });    // Non-finite value.
    rejectAndKeep({ {0.0f, 0.0f}, {0.5f, infinity}, {1.0f, 0.5f} });

    // Finite playTimes and clipped intervals cannot participate in a binding.
    Fixture limited;
    auto* onceWalk = limited.animation()->play("walk", 1);
    auto* loopRun = limited.animation()->play("run", 0);
    DB_CHECK(!loopRun->synchronizeTo(onceWalk, markers));
    DB_CHECK(!onceWalk->synchronizeTo(loopRun, markers));

    auto* intervalConfig = limited.animation()->getAnimationConfig();
    intervalConfig->clear();
    intervalConfig->animation = "walk";
    intervalConfig->playTimes = 0;
    intervalConfig->position = 0.1f;
    intervalConfig->duration = 0.3f;
    intervalConfig->fadeInTime = 0.0f;
    auto* intervalWalk = limited.animation()->playConfig(intervalConfig);
    DB_CHECK(!loopRun->synchronizeTo(intervalWalk, markers));

    // unsynchronize resumes from the last mapped time using the state's own timeScale.
    Fixture unbind;
    auto* unbindWalk = unbind.animation()->play("walk", 0);
    auto* unbindRun = unbind.playFollower();
    DB_CHECK(unbindRun->synchronizeTo(unbindWalk, markers));
    unbind.step(0.5f); // run mapped to 0.4
    unbindRun->unsynchronize();
    DB_CHECK(!unbindRun->isSynchronized());
    DB_CHECK(close(unbindRun->getCurrentTime(), 0.4f, 1e-6f));
    unbindRun->timeScale = 2.0f;
    unbind.step(0.1f);
    DB_CHECK(close(unbindRun->getCurrentTime(), 0.1f, 1e-5f)); // 0.4 + 0.1 * 2 = 0.6, wrapped to 0.1.

    // Legacy playback, layer-weight blending and fade in/out still behave as before.
    Fixture legacy;
    auto* legacyWalk = legacy.animation()->play("walk", 0);
    legacy.step(0.25f);
    DB_CHECK(close(legacy.rootTx(), 4.0f, 1e-4f)); // x goes 0 -> 8 across the first 0.5s.
    DB_CHECK(close(legacyWalk->getCurrentTime(), 0.25f, 1e-5f));

    auto* legacyRun = legacy.animation()->fadeIn("run", 0.2f, 0, 1, "", AnimationFadeOutMode::None);
    legacyRun->resetToPose = false;
    legacy.step(0.1f); // Fade in progress: the high layer contributes only partially.
    DB_CHECK(legacyRun->isFadeIn());
    DB_CHECK(legacy.rootTx() > 0.0f);

    legacy.step(0.2f); // Fade in completes; the high layer now owns the full weight.
    DB_CHECK(legacyRun->isFadeComplete());
    legacyRun->fadeOut(0.1f, false);
    legacy.step(0.2f); // The fade out completes during this frame.
    legacy.step(0.1f); // The finished state is collected at the start of the next frame.
    DB_CHECK(legacy.animation()->getState("run") == nullptr);
}
