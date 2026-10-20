# First-person viewmodel: findings and plan (2026-10-20)

Goal: a real first-person mode, like an FPS game. The camera sits at Ellie's eyes. Her arms and the gun are
drawn as a viewmodel: their own field of view, never poking into walls, held in view. Built as our own mod,
separate from the co-op mod (`TLOU2Viewmodel.dll`, loaded with `fp_launcher --asi`).

## 1. What the engine has

- No viewmodel and no first-person mode. The only first-person strings are Havok's (`hknpFirstPersonGun`).
- "Foreground" in this engine means dynamic objects, not a weapon layer.
- So there is no second render pass to switch on. The viewmodel has to come from moving the drawn mesh.

## 2. The mechanism (proven live 10:41-10:48)

Each character is skinned on the GPU from one 3x4 matrix per joint.

| Fact | Where |
|---|---|
| Matrices: rows of float4, translation in column 3 | AnimExecutionContext+112 |
| Count (1074 for Ellie) | skeleton (ctx+16) +68 |
| Context of an object | `sub_141696820(FgAnimData)`, FgAnimData = Process+448 |
| Space | the object's own space (feet at 0, y up); the object's locator puts it in the world |
| Filled by | PerformJointBlend, rva 0x17B5C00 (anim-mgr-jobs.cpp) |
| Then redone for arms and legs (IK) | Player::PostJointUpdate_Async, rva 0xDB9840 (vtable +1544) |

A hook after PostJointUpdate_Async rewrites player 0's matrices. Her drawn mesh moves. Her real position,
animation, collision and camera do not.

Live checks on PC A (Watchtower, third-person camera; frames grabbed with PrintWindow, game not in front):
1. 10:41, hook after PerformJointBlend, lift 0.6 m: torso and backpack rose, hands and feet stayed. The IK pass
   overwrote the limbs. That is why the hook moved to PostJointUpdate_Async.
2. 10:44, after PostJointUpdate_Async: lift 0.6 m moves all of her, hands and feet too. Scale 0.5 shrinks all of her.
3. 10:48, the viewmodel transform about the camera:
   - k 1, s 0.5: she looks the same but is drawn half-way to the camera. The tall grass no longer covers her.
   - k 1.5: she is drawn 1.5 times bigger about the screen centre (a narrower field of view).
   - The rifle on her back stayed put. Weapons are separate objects with their own matrices.

## 3. The viewmodel transform (src/core/viewmodel.h, tested)

About the eye, scale across the view direction by k * s and along it by s.
- From the eye, every point keeps its direction, with screen offsets times k. That is the look of a different
  field of view: k = tan(worldFov / 2) / tan(viewFov / 2).
- Every point comes s times closer. With s around 0.3 the gun stays inside the capsule and cannot poke into walls.
- It is affine, so it composes with the skinning matrices: new = L^-1 * A * L * m (L = the object's locator).
- Only the view direction matters (`ViewmodelAlong`).

## 4. Plan

| Phase | What | Size | Check |
|---|---|---|---|
| 1 | Held weapon: find it in the player's process subtree (walk rva 0x1448C10), take the one on the right hand, give it the same transform (its own locator) | small | the gun moves with the arms in a frame grab |
| 2 | Arms only: map skinning-matrix index to joint name (skeleton tables), apply the transform to the arm chain + hands, leave the body normal | medium | body unchanged, arms + gun drawn nearer |
| 3 | Own eye camera: CameraFinalInfo::Set rva 0x1473BA0; manager 0 holds the final camera at +0x130 (position), +0x140 (rotation quaternion), +0x160 (fov). Head meshes hidden from the main view only (DisableMeshFromMainView 0x147DF40), so their shadow stays. Near plane: native `camera-override-near-clip/f` 0xD356C0 | medium | replaces the third-party camera |
| 4 | Head-turn limit (the vault "neck break"): clamp the camera yaw and pitch to a cone around the chest joint's facing | small | vault with the camera turned back |
| 5 | Hip pose: a camera-space offset that holds the gun low-right in view, plus sway taken from the animation; aim: line the gun's sight up with the screen centre | large, tuning | owner eye checks |
| 6 | Settings, ini, toggle key, packaging next to fp_launcher | small | |

## 5. Open questions and risks

- The camera read in the joint update is last frame's. Fast turns may show a one-frame wobble of the gun. Fix:
  build the eye from this frame's head joint instead.
- Does the third-party mod's camera land in manager 0's final camera, or only in the render copy? Not checked.
- Shadows: squashed arms cast squashed shadows. Small near the body; check by eye.
- The co-op clone (player 1) also runs PostJointUpdate_Async; the hook only touches player 0.
- Matrix count: 1074 per frame, about 13 k multiply-adds. No measurable cost.
