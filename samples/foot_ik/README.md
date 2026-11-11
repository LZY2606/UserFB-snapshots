# Ozz-animation sample: Foot IK

## Description

Foot IK, or foot planting, is a technique used to correct character legs and ankles procedurally at runtime, as well as character/pelvis height, so that the feet can touch and adapt to the ground.

## Concept

The core concept is to apply two bone IK to the hip/knee/ankle joint chains of the character so each foot is at the correct height from the ground, and aim IK to the ankle so that it's aligned to the ground. Raycast (from ankle position, directed down to the ground) is used to find the point and normal where each foot should be on ground. Foot height (heel, sole) and slope angle must be considered when computing ankle position, to be sure the orientated foot remains precisely on ground. 
The second important aspect is to adjust character height, so that the lowest foot touches the ground. The over leg(s) will be bent by IK. 

This sample focuses on IK application and core maths. It's not a complete foot planting implementation, as it would require more:
- Activate/deactivate foot planting, blending in and out when feet are not supposed to be on ground, like during walk cycle.
- Detect not reachable foot positions: too high, too low, too steep... and deactivate IK.
- A layer of animation should also be added, to cope with major leg elevation differences. This would allow IK to correct more natural positions.
- ...

## Contact-track foot locking

By default the sample raycasts the ground from the animated ankle every frame: as soon as the character root moves horizontally or rotates, the foot target is recomputed, so a supporting foot cannot stay planted on one spot. An optional "contact-track" layer (`contact_lock.h`/`contact_lock.cc`, shared with a windowless regression) fixes that.

Each foot can be given a `FloatTrack` whose value is greater than `0.5` while the foot is supporting, and any other value while it is swinging:

- `--left_contact=<path/to/left_contact.ozz>`: left foot contact track.
- `--right_contact=<path/to/right_contact.ozz>`: right foot contact track.

Both options are optional and independent. A foot without a track keeps the original per-frame ground search, so running the sample without any contact track behaves exactly as before. Both step and linear tracks are accepted: threshold crossings (including linear ramps) are located with ozz's `TrackTriggeringJob`.

On a rising edge, the layer samples the base posture, the root transform and the ground hit at the exact edge time, then stores the resulting world-space ankle target and ground normal. That world anchor does not follow the character root, and later raycasts never overwrite it. Every frame the fixed anchor is transformed back into the current character model space and fed to the unchanged pelvis correction, two bone IK and ankle aim IK. Rules:

- A falling edge releases the lock immediately; a swinging foot gets no contact correction.
- If no ground is hit at the rising edge, the whole support phase runs unlocked.
- If the anchor later becomes unreachable, it is released and stays released until the next rising edge (no re-capture, no sliding).
- Manual time jumps, backward playback and animation switches reset the locks; past states are not reconstructed.
- Time uses the un-wrapped playback ratio, so an update crossing several loops processes every edge in order.

### Debug CSV

`--contact_csv=<path/to/debug.csv>` writes one row per frame with the un-wrapped playback ratio, the contact state, the anchor coordinates and the actual IK'd ankle coordinates of both feet. It only takes effect when at least one contact track is loaded. Example:

```
sample_foot_ik --left_contact=media/left_contact.ozz \
               --right_contact=media/right_contact.ozz \
               --contact_csv=contact_debug.csv
```

The columns are `ratio, left_contact, left_anchor_{x,y,z}, left_ankle_{x,y,z}, right_contact, right_anchor_{x,y,z}, right_ankle_{x,y,z}`. A locked foot whose anchor columns stay constant while its root moves confirms the planting; the ankle columns show how close the IK chain gets to that anchor. Locked anchors can also be visualized in the "Debug options" panel with "Show contact anchors".

A windowless, asset-free regression of this layer lives in `checks/contact_lock_regression.cc` and is run by `sh checks/check-contact-lock.sh` (see the file header in `checks/contact_lock_regression.cc`).


## Sample usage

The sample allows to move and rotate the character on the collision mesh, with y value being automatically computed if "auto character height" option is enabled.
Pelvis correction, two bone IK and aim IK can be activated independently.
Debug settings are exposed to help understanding: showing joints, raycasts, IK targets...

## Implementation

1. At initialization time:
   1. Loads skeleton, animation and mesh. See "playback" sample for more details.
   2. Locates hip, knee and ankle joints for each leg, searching them by name.
2. At run time:
   1. Updates base animation and skeleton joints model-space matrices. See "playback" sample for more details.
   2. Estimates character height on the floor, evaluted at its root position.
   3. For each leg, raycasts a vector going down from the ankle position. This allows to find the intersection point (I) with the floor.
   4. When contact tracks are provided, processes their rising/falling edges over the un-wrapped ratio range and captures or releases world-space anchors (see "Contact-track foot locking" above). Without tracks this step is skipped.
   5. Comptutes ankle target position (C), so that the foot is in contact with the floor. Because of floor slope (defined by raycast intersection normal), ankle position cannot be simply be offseted by foot offset. See diagram below.

![ankle correction](../../media/doc/samples/foot_ik_ankle.svg)

   6. Offsets the character down, so that the lowest ankle (lowest from its original position) reaches its targetted position. The other leg(s) will be ik-ed.
   7. Applies two bone IK to each leg, so the ankles reache their targetted position. This in computed in character model-space.
   8. Applies aim IK to each ankle, so the footis correctly aligned to the floor. Uses floor normal as a reference to align ankle up vector. The forward direction of the foot is then driven by the pole vector, which polls the foot (ankle forward vector) toward it's original (animated) direction.