#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <AL/al.h>
#include "al_listener.h"
#include "al_buffer.h"

#define EPSILON 1e-5f

static inline bool float_eq(float a, float b) {
    return fabsf(a - b) < EPSILON;
}

static inline bool vec3_eq(const float a[3], float x, float y, float z) {
    return float_eq(a[0], x) && float_eq(a[1], y) && float_eq(a[2], z);
}

static inline float vec3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline float vec3_len(const float a[3]) {
    return sqrtf(vec3_dot(a, a));
}

int main(void) {
    printf("=== OpenAL Listener Vector Math & Coordinate Basis Host Test ===\n");

    /* Subsystem initialization */
    al_listener_init();
    assert(alGetError() == AL_NO_ERROR);

    /*
     * ------------------------------------------------------------------------
     * Test 1: Default Listener State & Basis Orthonormality
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing default listener basis vectors & state...\n");
    float fwd[3] = {0}, up[3] = {0}, right[3] = {0};
    al_listener_get_basis(fwd, up, right);

    /* Default basis: Forward (0,0,-1), Up (0,1,0), Right (1,0,0) */
    assert(vec3_eq(fwd, 0.0f, 0.0f, -1.0f));
    assert(vec3_eq(up, 0.0f, 1.0f, 0.0f));
    assert(vec3_eq(right, 1.0f, 0.0f, 0.0f));

    /* Unit lengths */
    assert(float_eq(vec3_len(fwd), 1.0f));
    assert(float_eq(vec3_len(up), 1.0f));
    assert(float_eq(vec3_len(right), 1.0f));

    /* Mutual orthogonality: all dot products must equal 0 */
    assert(float_eq(vec3_dot(fwd, right), 0.0f));
    assert(float_eq(vec3_dot(fwd, up), 0.0f));
    assert(float_eq(vec3_dot(right, up), 0.0f));

    /* Check getters */
    float pos[3] = {0}, vel[3] = {0};
    al_listener_get_position(pos);
    assert(vec3_eq(pos, 0.0f, 0.0f, 0.0f));

    al_listener_get_velocity(vel);
    assert(vec3_eq(vel, 0.0f, 0.0f, 0.0f));

    assert(float_eq(al_listener_get_gain(), 1.0f));

    ALfloat q_ori[6] = {0};
    alGetListenerfv(AL_ORIENTATION, q_ori);
    assert(vec3_eq(&q_ori[0], 0.0f, 0.0f, -1.0f));
    assert(vec3_eq(&q_ori[3], 0.0f, 1.0f, 0.0f));

    ALfloat q_pos[3] = {0}, q_vel[3] = {0};
    alGetListenerfv(AL_POSITION, q_pos);
    assert(vec3_eq(q_pos, 0.0f, 0.0f, 0.0f));
    alGetListenerfv(AL_VELOCITY, q_vel);
    assert(vec3_eq(q_vel, 0.0f, 0.0f, 0.0f));

    ALfloat q_gain = 0.0f;
    alGetListenerf(AL_GAIN, &q_gain);
    assert(float_eq(q_gain, 1.0f));
    printf("    -> Default basis: F(0,0,-1), U(0,1,0), R(1,0,0) [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 2: World to Local Transformation at Default Position & Orientation
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing world to local transform at default position & orientation...\n");
    float local[3] = {0};

    /* Front: (0, 0, -5) -> local (0, 0, -5) [spec criterion 36] */
    const float pt_front[3] = {0.0f, 0.0f, -5.0f};
    al_listener_world_to_local(pt_front, local);
    assert(vec3_eq(local, 0.0f, 0.0f, -5.0f));

    /* Behind: (0, 0, 5) -> local (0, 0, 5) */
    const float pt_behind[3] = {0.0f, 0.0f, 5.0f};
    al_listener_world_to_local(pt_behind, local);
    assert(vec3_eq(local, 0.0f, 0.0f, 5.0f));

    /* Right: (3, 0, 0) -> local (3, 0, 0) */
    const float pt_right[3] = {3.0f, 0.0f, 0.0f};
    al_listener_world_to_local(pt_right, local);
    assert(vec3_eq(local, 3.0f, 0.0f, 0.0f));

    /* Left: (-3, 0, 0) -> local (-3, 0, 0) */
    const float pt_left[3] = {-3.0f, 0.0f, 0.0f};
    al_listener_world_to_local(pt_left, local);
    assert(vec3_eq(local, -3.0f, 0.0f, 0.0f));

    /* Above: (0, 4, 0) -> local (0, 4, 0) */
    const float pt_up[3] = {0.0f, 4.0f, 0.0f};
    al_listener_world_to_local(pt_up, local);
    assert(vec3_eq(local, 0.0f, 4.0f, 0.0f));

    /* Below: (0, -4, 0) -> local (0, -4, 0) */
    const float pt_down[3] = {0.0f, -4.0f, 0.0f};
    al_listener_world_to_local(pt_down, local);
    assert(vec3_eq(local, 0.0f, -4.0f, 0.0f));

    /* In-place transformation */
    float pt_inplace[3] = {2.0f, 3.0f, -4.0f};
    al_listener_world_to_local(pt_inplace, pt_inplace);
    assert(vec3_eq(pt_inplace, 2.0f, 3.0f, -4.0f));
    printf("    -> World to local projection at origin [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 3: Listener Translation (Non-Origin Position)
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing listener position translation...\n");
    alListener3f(AL_POSITION, 10.0f, 20.0f, 30.0f);
    assert(alGetError() == AL_NO_ERROR);

    al_listener_get_position(pos);
    assert(vec3_eq(pos, 10.0f, 20.0f, 30.0f));

    /* Sound at listener position -> (0, 0, 0) */
    const float pt_at_listener[3] = {10.0f, 20.0f, 30.0f};
    al_listener_world_to_local(pt_at_listener, local);
    assert(vec3_eq(local, 0.0f, 0.0f, 0.0f));

    /* Sound 5 units in front (depth -Z in world): (10, 20, 25) -> (0, 0, -5) */
    const float pt_translated_front[3] = {10.0f, 20.0f, 25.0f};
    al_listener_world_to_local(pt_translated_front, local);
    assert(vec3_eq(local, 0.0f, 0.0f, -5.0f));

    /* Sound 5 units to the right (+X in world): (15, 20, 30) -> (5, 0, 0) */
    const float pt_translated_right[3] = {15.0f, 20.0f, 30.0f};
    al_listener_world_to_local(pt_translated_right, local);
    assert(vec3_eq(local, 5.0f, 0.0f, 0.0f));

    /* Sound 5 units above (+Y in world): (10, 25, 30) -> (0, 5, 0) */
    const float pt_translated_up[3] = {10.0f, 25.0f, 30.0f};
    al_listener_world_to_local(pt_translated_up, local);
    assert(vec3_eq(local, 0.0f, 5.0f, 0.0f));

    /* Velocity setting */
    alListener3f(AL_VELOCITY, 5.0f, -10.0f, 15.0f);
    assert(alGetError() == AL_NO_ERROR);
    al_listener_get_velocity(vel);
    assert(vec3_eq(vel, 5.0f, -10.0f, 15.0f));
    printf("    -> Listener translation and velocity updates [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 4: Custom Orientation - Facing East (+X yaw)
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing custom orientation: Facing East (+X)...\n");
    /* Reset position to origin */
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);

    /* Orientation: Forward = (+1, 0, 0), Up = (0, 1, 0) */
    const ALfloat ori_east[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_east);
    assert(alGetError() == AL_NO_ERROR);

    al_listener_get_basis(fwd, up, right);
    /* Forward: (1, 0, 0), Up: (0, 1, 0), Right = F x U = (0, 0, 1) */
    assert(vec3_eq(fwd, 1.0f, 0.0f, 0.0f));
    assert(vec3_eq(up, 0.0f, 1.0f, 0.0f));
    assert(vec3_eq(right, 0.0f, 0.0f, 1.0f));

    /* Orthonormality check */
    assert(float_eq(vec3_len(fwd), 1.0f));
    assert(float_eq(vec3_len(up), 1.0f));
    assert(float_eq(vec3_len(right), 1.0f));
    assert(float_eq(vec3_dot(fwd, right), 0.0f));
    assert(float_eq(vec3_dot(fwd, up), 0.0f));
    assert(float_eq(vec3_dot(right, up), 0.0f));

    /* Sound at (5, 0, 0) must transform to local front (0, 0, -5) [spec criterion 37] */
    const float pt_east_front[3] = {5.0f, 0.0f, 0.0f};
    al_listener_world_to_local(pt_east_front, local);
    assert(vec3_eq(local, 0.0f, 0.0f, -5.0f));

    /* Sound at (0, 0, 5) is to the listener's right (+Z in world) -> local (5, 0, 0) */
    const float pt_east_right[3] = {0.0f, 0.0f, 5.0f};
    al_listener_world_to_local(pt_east_right, local);
    assert(vec3_eq(local, 5.0f, 0.0f, 0.0f));

    /* Sound at (-5, 0, 0) is behind the listener -> local (0, 0, 5) */
    const float pt_east_back[3] = {-5.0f, 0.0f, 0.0f};
    al_listener_world_to_local(pt_east_back, local);
    assert(vec3_eq(local, 0.0f, 0.0f, 5.0f));

    /* Sound at (0, 0, -5) is to the listener's left (-Z in world) -> local (-5, 0, 0) */
    const float pt_east_left[3] = {0.0f, 0.0f, -5.0f};
    al_listener_world_to_local(pt_east_left, local);
    assert(vec3_eq(local, -5.0f, 0.0f, 0.0f));

    /* Sound at (0, 5, 0) is above the listener -> local (0, 5, 0) */
    const float pt_east_up[3] = {0.0f, 5.0f, 0.0f};
    al_listener_world_to_local(pt_east_up, local);
    assert(vec3_eq(local, 0.0f, 5.0f, 0.0f));
    printf("    -> Facing East (+X): sound at (5,0,0) transformed to local (0,0,-5) [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 5: Custom Orientation - Facing North (+Z yaw)
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing custom orientation: Facing North (+Z)...\n");
    /* Orientation: Forward = (0, 0, 1), Up = (0, 1, 0) */
    const ALfloat ori_north[6] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_north);
    assert(alGetError() == AL_NO_ERROR);

    al_listener_get_basis(fwd, up, right);
    /* Forward: (0, 0, 1), Up: (0, 1, 0), Right = F x U = (-1, 0, 0) */
    assert(vec3_eq(fwd, 0.0f, 0.0f, 1.0f));
    assert(vec3_eq(up, 0.0f, 1.0f, 0.0f));
    assert(vec3_eq(right, -1.0f, 0.0f, 0.0f));

    /* Orthonormality check */
    assert(float_eq(vec3_len(fwd), 1.0f));
    assert(float_eq(vec3_len(up), 1.0f));
    assert(float_eq(vec3_len(right), 1.0f));
    assert(float_eq(vec3_dot(fwd, right), 0.0f));
    assert(float_eq(vec3_dot(fwd, up), 0.0f));
    assert(float_eq(vec3_dot(right, up), 0.0f));

    /* Sound at (0, 0, 5) -> local front (0, 0, -5) */
    const float pt_north_front[3] = {0.0f, 0.0f, 5.0f};
    al_listener_world_to_local(pt_north_front, local);
    assert(vec3_eq(local, 0.0f, 0.0f, -5.0f));

    /* Sound at (-5, 0, 0) is to the right of listener facing +Z -> local (5, 0, 0) */
    const float pt_north_right[3] = {-5.0f, 0.0f, 0.0f};
    al_listener_world_to_local(pt_north_right, local);
    assert(vec3_eq(local, 5.0f, 0.0f, 0.0f));
    printf("    -> Facing North (+Z): basis & local transforms verified [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 6: Custom Orientation - Looking Up (+Y pitch)
     * ------------------------------------------------------------------------
     */
    printf("[6] Testing pitch rotation: Looking Up (+Y)...\n");
    /* Forward: (0, 1, 0), Up: (0, 0, 1) */
    const ALfloat ori_up[6] = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    alListenerfv(AL_ORIENTATION, ori_up);
    assert(alGetError() == AL_NO_ERROR);

    al_listener_get_basis(fwd, up, right);
    /* Right = F x U = (1, 0, 0) */
    assert(vec3_eq(fwd, 0.0f, 1.0f, 0.0f));
    assert(vec3_eq(up, 0.0f, 0.0f, 1.0f));
    assert(vec3_eq(right, 1.0f, 0.0f, 0.0f));

    /* Orthonormality check */
    assert(float_eq(vec3_len(fwd), 1.0f));
    assert(float_eq(vec3_len(up), 1.0f));
    assert(float_eq(vec3_len(right), 1.0f));
    assert(float_eq(vec3_dot(fwd, right), 0.0f));
    assert(float_eq(vec3_dot(fwd, up), 0.0f));
    assert(float_eq(vec3_dot(right, up), 0.0f));

    /* Sound above listener at (0, 5, 0) is in front of the gaze -> local (0, 0, -5) */
    const float pt_pitch_front[3] = {0.0f, 5.0f, 0.0f};
    al_listener_world_to_local(pt_pitch_front, local);
    assert(vec3_eq(local, 0.0f, 0.0f, -5.0f));
    printf("    -> Looking Up (+Y): basis & pitch transforms verified [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 7: Gram-Schmidt Orthogonalization (Non-Normalized / Non-Orthogonal)
     * ------------------------------------------------------------------------
     */
    printf("[7] Testing Gram-Schmidt orthogonalization on unnormalized inputs...\n");
    /* Forward unnormalized: (0, 0, -10), Up unnormalized: (0, 20, 0) */
    const ALfloat ori_unnorm[6] = {0.0f, 0.0f, -10.0f, 0.0f, 20.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_unnorm);
    assert(alGetError() == AL_NO_ERROR);

    al_listener_get_basis(fwd, up, right);
    assert(vec3_eq(fwd, 0.0f, 0.0f, -1.0f));
    assert(vec3_eq(up, 0.0f, 1.0f, 0.0f));
    assert(vec3_eq(right, 1.0f, 0.0f, 0.0f));

    /* Non-orthogonal Up: Forward = (0, 0, -1), Up tilted 45 deg = (0, 1, -1) */
    const ALfloat ori_tilted[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, -1.0f};
    alListenerfv(AL_ORIENTATION, ori_tilted);
    assert(alGetError() == AL_NO_ERROR);

    al_listener_get_basis(fwd, up, right);
    /* U' must be strictly orthogonalized to (0, 1, 0) */
    assert(vec3_eq(fwd, 0.0f, 0.0f, -1.0f));
    assert(vec3_eq(up, 0.0f, 1.0f, 0.0f));
    assert(vec3_eq(right, 1.0f, 0.0f, 0.0f));

    /* Arbitrary 3D orientation diagonal test */
    const ALfloat ori_arbitrary[6] = {1.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f};
    alListenerfv(AL_ORIENTATION, ori_arbitrary);
    assert(alGetError() == AL_NO_ERROR);

    al_listener_get_basis(fwd, up, right);
    assert(float_eq(vec3_len(fwd), 1.0f));
    assert(float_eq(vec3_len(up), 1.0f));
    assert(float_eq(vec3_len(right), 1.0f));
    assert(fabsf(vec3_dot(fwd, right)) < EPSILON);
    assert(fabsf(vec3_dot(fwd, up)) < EPSILON);
    assert(fabsf(vec3_dot(right, up)) < EPSILON);
    printf("    -> Gram-Schmidt orthogonalization guarantees unit length & zero dot product [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 8: OpenAL 1.1 Error Semantics & Degenerate Orientation Handling
     * ------------------------------------------------------------------------
     */
    printf("[8] Testing OpenAL 1.1 error semantics & degenerate orientation cases...\n");
    /* NULL pointer to alListenerfv */
    alListenerfv(AL_ORIENTATION, NULL);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Zero-length Forward vector */
    const ALfloat ori_zero_fwd[6] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_zero_fwd);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Collinear Forward and Up vectors (F || U) */
    const ALfloat ori_collinear[6] = {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_collinear);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Opposite collinear Forward and Up vectors */
    const ALfloat ori_opp_collinear[6] = {0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_opp_collinear);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Non-finite values (NaN / Inf) */
    const ALfloat ori_nan[6] = {0.0f, 0.0f, NAN, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_nan);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Invalid getters */
    alGetListenerfv(AL_ORIENTATION, NULL);
    assert(alGetError() == AL_INVALID_VALUE);

    alGetListener3f(AL_POSITION, NULL, NULL, NULL);
    assert(alGetError() == AL_INVALID_VALUE);

    alGetListenerf(AL_GAIN, NULL);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Invalid enum */
    alListenerf(0x7FFF, 1.0f);
    assert(alGetError() == AL_INVALID_ENUM);

    alListener3f(AL_ORIENTATION, 0.0f, 0.0f, 0.0f);
    assert(alGetError() == AL_INVALID_ENUM);

    /* Reset listener state to defaults */
    al_listener_reset();
    al_listener_get_basis(fwd, up, right);
    assert(vec3_eq(fwd, 0.0f, 0.0f, -1.0f));
    assert(vec3_eq(up, 0.0f, 1.0f, 0.0f));
    assert(vec3_eq(right, 1.0f, 0.0f, 0.0f));
    printf("    -> Error semantics and reset confirmed [PASS]\n");

    printf("=== All Listener Vector Math Tests Passed Successfully! ===\n");
    return 0;
}
