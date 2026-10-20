/* Game logic - a port of the Expo project's GameEngine.ts, CrossyGame.ts,
 * CrossyPlayer.ts, the Row and Particles modules.
 *
 * Timing: the original moves cars/logs/trains a fixed amount per animation
 * frame (60 Hz) and runs GSAP tweens in seconds. Here everything runs on a
 * fixed 1/60 s step, so behaviour is identical regardless of render speed.
 *
 * Deliberate fixes vs. the original (marked FIX below):
 *  - new hop cancels the previous hop's tweens/callback (the original let
 *    the old onComplete clear `moving` mid-hop on rapid input)
 *  - turning takes the shortest way round (original could spin 270 deg)
 *  - train collision box matches the train's real extent
 *  - restart resets the camera instead of panning back from the old spot
 *  - getting run over by a train plays a sound (original was silent)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "game.h"
#include "render.h"
#include "audio.h"
#include "platform.h"
#include "assets_data.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define PI_F 3.14159265f
#define PI_2 (PI_F * 0.5f)
#define DT (1.0f / 60.0f)

/* GameSettings.ts */
#define GROUND_LEVEL 0.4f
#define SCENE_COLOR 0x87C6FF
#define STARTING_ROW 8
#define MAX_ROWS 20
#define CAMERA_EASING 0.03f
#define MAP_OFFSET (-30.0f)
#define BASE_ANIM 0.1f
#define PLAYER_IDLE_SCALE 0.8f
#define HERO_WIDTH 0.7f
#define VIEW_HALF_HEIGHT 2.9f

typedef struct { float x, y, z; } V3;

/* ------------------------------------------------------------ random */
static unsigned int rng = 0x12345678u;
static void rng_seed(unsigned int s) { rng = s ? s : 0x9e3779b9u; }
static float frand(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (rng >> 8) * (1.0f / 16777216.0f);
}
static float js_round(float x) { return floorf(x + 0.5f); }
static float normalize_angle(float a) { return atan2f(sinf(a), cosf(a)); }

/* ------------------------------------------------------------ tweens */
enum { E_LINEAR, E_QUAD_OUT, E_QUAD_IN, E_QUAD_INOUT, E_CUBIC_IN, E_CUBIC_INOUT, E_BOUNCE_OUT };
enum { G_NONE = 0, G_HOP = 1 };
typedef void (*TweenCb)(int arg);

typedef struct {
    float *target;
    float from, to, dur, delay, t;
    unsigned char ease, active, started, group;
    TweenCb cb;
    int arg;
} Tween;

#define MAX_TWEENS 512
static Tween tweens[MAX_TWEENS];

static float ease_fn(int e, float p) {
    switch (e) {
    case E_QUAD_OUT: return 1 - (1 - p) * (1 - p);
    case E_QUAD_IN: return p * p;
    case E_QUAD_INOUT: return p < 0.5f ? 2 * p * p : 1 - 2 * (1 - p) * (1 - p);
    case E_CUBIC_IN: return p * p * p;
    case E_CUBIC_INOUT: return p < 0.5f ? 4 * p * p * p : 1 - 4 * (1 - p) * (1 - p) * (1 - p);
    case E_BOUNCE_OUT:
        if (p < 1 / 2.75f) return 7.5625f * p * p;
        if (p < 2 / 2.75f) { p -= 1.5f / 2.75f; return 7.5625f * p * p + 0.75f; }
        if (p < 2.5f / 2.75f) { p -= 2.25f / 2.75f; return 7.5625f * p * p + 0.9375f; }
        p -= 2.625f / 2.75f; return 7.5625f * p * p + 0.984375f;
    default: return p;
    }
}

/* GSAP-style "to" tween. The start value is captured when the delay ends,
   and (like GSAP's auto-overwrite) starting a tween kills other running
   tweens on the same property. Default ease = Power1.easeOut, like GSAP 2. */
static Tween *tween_to(float *target, float to, float dur, float delay, int ease, int group) {
    int i;
    for (i = 0; i < MAX_TWEENS; i++) {
        if (!tweens[i].active) {
            Tween *t = &tweens[i];
            memset(t, 0, sizeof(*t));
            t->target = target; t->to = to; t->dur = dur > 0 ? dur : 1e-4f;
            t->delay = delay; t->ease = (unsigned char)ease; t->active = 1; t->group = (unsigned char)group;
            return t;
        }
    }
    return 0;
}

static void tween_kill_group(int group) {
    int i;
    for (i = 0; i < MAX_TWEENS; i++) if (tweens[i].active && tweens[i].group == group) tweens[i].active = 0;
}
static void tween_kill_target(float *target) {
    int i;
    for (i = 0; i < MAX_TWEENS; i++) if (tweens[i].active && tweens[i].target == target) tweens[i].active = 0;
}
static void tween_kill_all(void) { memset(tweens, 0, sizeof(tweens)); }

static void tweens_update(float dt) {
    int i, j;
    for (i = 0; i < MAX_TWEENS; i++) {
        Tween *t = &tweens[i];
        if (!t->active) continue;
        if (!t->started) {
            t->delay -= dt;
            if (t->delay > 0) continue;
            for (j = 0; j < MAX_TWEENS; j++)
                if (j != i && tweens[j].active && tweens[j].started && tweens[j].target == t->target) tweens[j].active = 0;
            t->started = 1;
            t->from = *t->target;
            t->t = -t->delay; /* carry over the overshoot */
        } else {
            t->t += dt;
        }
        {
            float p = t->t / t->dur;
            if (p >= 1) p = 1;
            *t->target = t->from + (t->to - t->from) * ease_fn(t->ease, p);
            if (p >= 1) {
                t->active = 0;
                if (t->cb) t->cb(t->arg);
            }
        }
    }
}

/* ------------------------------------------------------------ world data */
enum { ROW_GRASS, ROW_ROAD, ROW_WATER, ROW_RAIL };

typedef struct {
    const Mesh *mesh;
    V3 pos;
    float rot_y;
    float speed;
    int dir;
    float width, collision;
    float top, min, mid;          /* water ridables */
    float lily_amp, lily_period, lily_t;
} Entity;

typedef struct {
    float x, z, s, rot_y;
    float t;          /* time into current phase */
    float delay;
    float x_from, x_to;
    int phase;        /* 0 wait, 1 grow, 2 shrink+drift */
} FoamPart;

typedef struct {
    int type;
    float z;
    int active;
    /* grass */
    int nobs;
    int obs_x[16];
    const Mesh *obs_mesh[16];
    int clear[16]; int nclear;
    /* road / water */
    Entity ents[4];
    int nents;
    int first_lane;
    float sine_count;
    int lily_pos[4]; int nlily;
    FoamPart foam[2][6];
    /* rail */
    Entity train;
    int train_mids;
    float train_minx, train_maxx;
    int ringing, ring_count, light_a, light_b, light_off;
    float ring_timer;
} Row;

static Row grass_rows[MAX_ROWS], water_rows[MAX_ROWS], road_rows[MAX_ROWS], rail_rows[MAX_ROWS];
static int grass_count, water_count, road_count, rail_count, row_count;

#define MAP_SIZE 128
static struct { int index; Row *row; } floor_map[MAP_SIZE];

static Row *get_row(int index) {
    int k = index & (MAP_SIZE - 1);
    if (floor_map[k].row && floor_map[k].index == index) return floor_map[k].row;
    return 0;
}
static void set_row(int index, Row *r) {
    int k = index & (MAP_SIZE - 1);
    floor_map[k].index = index;
    floor_map[k].row = r;
}

static float row_top(const Row *r) {
    switch (r->type) {
    case ROW_GRASS: return 0.4f;
    case ROW_ROAD: return 0.3f;
    case ROW_WATER: return 0.25f;
    default: return 0.5f;
    }
}
static float row_floor_y(const Row *r) {
    switch (r->type) {
    case ROW_GRASS: return 0.375f;
    case ROW_ROAD: return 0.25f;
    case ROW_WATER: return 0.125f;
    default: return 0.375f;
    }
}

/* ------------------------------------------------------------ player */
typedef struct {
    V3 pos, scale, rot;
    V3 initial, target;
    int has_initial;
    float target_rot;
    int moving, alive;
    Entity *riding_on;
    float riding_on_offset;
    Entity *hit_by;
    int idle;
    float idle_t;
} Player;

static Player hero;
static float hero_pre_scale, hero_pre_off[3];

/* ------------------------------------------------------------ particles */
typedef struct { V3 pos, rot, scale; int visible; float t, dur; V3 c1, c2, end; int ease; } Particle;
static Particle feathers[20], water_parts[15];
static V3 feathers_origin, water_origin;

/* ------------------------------------------------------------ scene/camera */
static V3 world_pos;         /* scene.world.position */
static V3 scene_pos;         /* scene.position (rumble) */
static float cam_count;

/* ------------------------------------------------------------ meta state */
enum { GS_HOME, GS_PLAYING, GS_GAMEOVER, GS_PAUSED };
static int game_state, paused_from;
static int score, highscore, new_highscore;
static float state_time, title_anim, toast_time;
static char toast[32];
static int move_sound_index;
static int queued_dir = -1;   /* hop requested mid-air, run on landing */
static int rumble_frames;
static unsigned int prev_dir_held;

static void game_over(void);
static void save_settings(void);
static int show_fps;

static const Mesh *const car_meshes[8] = {
    &mesh_car_police, &mesh_car_blue, &mesh_truck_blue, &mesh_car_green,
    &mesh_car_orange, &mesh_car_purple, &mesh_truck_red, &mesh_car_taxi };
static const Mesh *const log_meshes[4] = { &mesh_log0, &mesh_log1, &mesh_log2, &mesh_log3 };
static const Mesh *const tree_meshes[4] = { &mesh_tree0, &mesh_tree1, &mesh_tree2, &mesh_tree3 };
static const Mesh *const boulder_meshes[2] = { &mesh_boulder0, &mesh_boulder1 };

/* ============================================================ effects */

static void rumble(void) {
    /* CrossyScene.rumble: shove the scene back one unit and return */
    tween_to(&scene_pos.z, 1, 0.2f, 0, E_QUAD_OUT, G_NONE);
    tween_to(&scene_pos.z, 0, 0.2f, 0.2f, E_QUAD_OUT, G_NONE);
    rumble_frames = 24; /* Vibration.vibrate() -> DualShock 3 motor */
}

static void play_death_sound(void) { audio_play(frand() < 0.5f ? SND_CHICKEN_DIE0 : SND_CHICKEN_DIE1, 1); }

static V3 bez(V3 a, V3 b, V3 c, V3 d, float u) {
    float iu = 1 - u;
    V3 r;
    float k0 = iu * iu * iu, k1 = 3 * iu * iu * u, k2 = 3 * iu * u * u, k3 = u * u * u;
    r.x = a.x * k0 + b.x * k1 + c.x * k2 + d.x * k3;
    r.y = a.y * k0 + b.y * k1 + c.y * k2 + d.y * k3;
    r.z = a.z * k0 + b.z * k1 + c.z * k2 + d.z * k3;
    return r;
}

static void hide_particle(int idx) {
    Particle *p = (idx >= 100) ? &water_parts[idx - 100] : &feathers[idx];
    p->visible = 0;
}

static void run_feathers(V3 at, float direction) {
    int i;
    const float explosion_speed = 0.3f;
    feathers_origin = at;
    for (i = 0; i < 20; i++) {
        Particle *p = &feathers[i];
        float m = direction < 0 ? -1.f : 1.f;
        float tx = (frand() * 5 + 2) * m, ty = frand() * 2 + 1, tz = frand() * 2 + 1;
        float delay = explosion_speed + frand() * 0.5f;
        Tween *t;
        memset(p, 0, sizeof(*p));
        p->c1.x = tx * 0.25f; p->c1.y = ty * 0.25f; p->c1.z = tz * 0.25f;
        p->c2.x = tx * 0.5f;  p->c2.y = ty * 0.5f;  p->c2.z = tz * 0.5f;
        p->end.x = tx; p->end.y = ty; p->end.z = tz;
        p->scale.x = p->scale.y = p->scale.z = 1;
        p->visible = 1; p->dur = delay * 5; p->ease = E_QUAD_OUT;
        tween_to(&p->rot.z, frand() * 2 * PI_F + 0.2f, delay * 5, delay, E_QUAD_OUT, G_NONE);
        tween_to(&p->rot.x, frand() * 2 * PI_F + 0.2f, delay * 5, delay, E_QUAD_OUT, G_NONE);
        tween_to(&p->rot.y, frand() * 2 * PI_F + 0.2f, delay * 5, delay, E_QUAD_OUT, G_NONE);
        tween_to(&p->scale.x, 0.01f, delay, delay * 3, E_QUAD_OUT, G_NONE);
        tween_to(&p->scale.y, 0.01f, delay, delay * 3, E_QUAD_OUT, G_NONE);
        t = tween_to(&p->scale.z, 0.01f, delay, delay * 3, E_QUAD_OUT, G_NONE);
        if (t) { t->cb = hide_particle; t->arg = i; }
    }
}

static void run_water(V3 at) {
    int i;
    const float explosion_speed = 0.3f;
    water_origin = at;
    for (i = 0; i < 15; i++) {
        Particle *p = &water_parts[i];
        float tx = -1 + frand(), ty = frand() * 2 + 1, tz = -1 + frand();
        float s = explosion_speed + frand() * 0.5f;
        Tween *t;
        memset(p, 0, sizeof(*p));
        p->c1.x = tx; p->c1.y = ty; p->c1.z = tz;
        p->c2.x = tx * 0.8f; p->c2.y = ty * 0.8f; p->c2.z = tz * 0.8f;
        p->end.x = tx * (frand() * 0.5f + 1.1f); p->end.y = 0; p->end.z = tz * (frand() * 0.5f + 1.1f);
        p->scale.x = p->scale.y = p->scale.z = 1;
        p->visible = 1; p->dur = s * 4; p->ease = E_BOUNCE_OUT;
        tween_to(&p->scale.x, 0.01f, s, s * 3, E_QUAD_OUT, G_NONE);
        tween_to(&p->scale.y, 0.01f, s, s * 3, E_QUAD_OUT, G_NONE);
        t = tween_to(&p->scale.z, 0.01f, s, s * 3, E_QUAD_OUT, G_NONE);
        if (t) { t->cb = hide_particle; t->arg = 100 + i; }
    }
    audio_play(SND_WATER, 1);
}

static void particles_update(void) {
    int i;
    V3 zero = { 0, 0, 0 };
    for (i = 0; i < 20; i++) {
        Particle *p = &feathers[i];
        if (!p->visible) continue;
        p->t += DT;
        p->pos = bez(zero, p->c1, p->c2, p->end, ease_fn(p->ease, p->t >= p->dur ? 1 : p->t / p->dur));
    }
    for (i = 0; i < 15; i++) {
        Particle *p = &water_parts[i];
        if (!p->visible) continue;
        p->t += DT;
        p->pos = bez(zero, p->c1, p->c2, p->end, ease_fn(p->ease, p->t >= p->dur ? 1 : p->t / p->dur));
    }
}

/* ============================================================ player */

static void player_reset(void) {
    memset(&hero, 0, sizeof(hero));
    hero.pos.x = 0; hero.pos.y = GROUND_LEVEL; hero.pos.z = STARTING_ROW;
    hero.scale.x = hero.scale.y = hero.scale.z = 1;
    hero.rot.y = PI_F;
    hero.alive = 1;
}

static void player_stop_idle(void) {
    hero.idle = 0;
    tween_kill_target(&hero.scale.y);
    hero.scale.x = hero.scale.y = hero.scale.z = 1;
}

static void player_idle_start(void) {
    if (hero.idle) return;
    player_stop_idle();
    hero.idle = 1;
    hero.idle_t = 0;
}

static void player_idle_update(void) {
    /* PlayerIdleAnimation: y 1 -> 0.8 (Power1.easeIn, 0.3s) -> 1 (Power1.easeOut, 0.3s), forever */
    float t;
    if (!hero.idle) return;
    hero.idle_t += DT;
    t = fmodf(hero.idle_t, 0.6f);
    if (t < 0.3f) hero.scale.y = 1 + (PLAYER_IDLE_SCALE - 1) * ease_fn(E_QUAD_IN, t / 0.3f);
    else hero.scale.y = PLAYER_IDLE_SCALE + (1 - PLAYER_IDLE_SCALE) * ease_fn(E_QUAD_OUT, (t - 0.3f) / 0.3f);
}

static void update_score(void) {
    int pos = (int)floorf(hero.pos.z) - STARTING_ROW;
    if (pos < 0) pos = 0;
    if (pos > score) score = pos;
}

static void on_hop_complete(int unused) {
    (void)unused;
    hero.moving = 0;     /* finishedMovingAnimation */
    update_score();
}

static void on_rot_complete(int unused) {
    (void)unused;
    hero.rot.y = normalize_angle(hero.rot.y);
}

static void player_stop_animations(void) { tween_kill_group(G_HOP); }

static void player_skip_pending_movement(void) {
    if (!hero.moving) return;
    hero.pos = hero.target;
    hero.rot.y = normalize_angle(hero.target_rot);
}

static void player_commit_movement(void) {
    V3 d, air;
    Tween *t;
    float rot_to;
    /* FIX: a new hop replaces the previous hop entirely */
    player_stop_animations();

    d.x = hero.target.x - hero.initial.x;
    d.z = hero.target.z - hero.initial.z;
    air.x = hero.initial.x + d.x * 0.75f;
    air.y = hero.target.y + 0.5f;
    air.z = hero.initial.z + d.z * 0.75f;

    /* PlayerPositionAnimation */
    tween_to(&hero.pos.x, air.x, BASE_ANIM, 0, E_QUAD_OUT, G_HOP);
    tween_to(&hero.pos.y, air.y, BASE_ANIM, 0, E_QUAD_OUT, G_HOP);
    tween_to(&hero.pos.z, air.z, BASE_ANIM, 0, E_QUAD_OUT, G_HOP);
    tween_to(&hero.pos.x, hero.target.x, BASE_ANIM, BASE_ANIM, E_QUAD_OUT, G_HOP);
    tween_to(&hero.pos.y, hero.target.y, BASE_ANIM, BASE_ANIM, E_QUAD_OUT, G_HOP);
    t = tween_to(&hero.pos.z, hero.target.z, BASE_ANIM, BASE_ANIM, E_QUAD_OUT, G_HOP);
    if (t) t->cb = on_hop_complete;

    /* PlayerScaleAnimation */
    tween_to(&hero.scale.x, 1, BASE_ANIM, 0, E_QUAD_OUT, G_HOP);
    tween_to(&hero.scale.y, 1.2f, BASE_ANIM, 0, E_QUAD_OUT, G_HOP);
    tween_to(&hero.scale.z, 1, BASE_ANIM, 0, E_QUAD_OUT, G_HOP);
    tween_to(&hero.scale.y, 0.8f, BASE_ANIM, BASE_ANIM, E_QUAD_OUT, G_HOP);
    tween_to(&hero.scale.y, 1, BASE_ANIM, BASE_ANIM * 2, E_BOUNCE_OUT, G_HOP);

    /* rotation, FIX: shortest way round */
    rot_to = hero.rot.y + normalize_angle(hero.target_rot - hero.rot.y);
    t = tween_to(&hero.rot.y, rot_to, BASE_ANIM, 0, E_QUAD_INOUT, G_HOP);
    if (t) t->cb = on_rot_complete;

    hero.initial = hero.target;
}

static void player_posie(void) {
    player_stop_idle();
    tween_to(&hero.scale.x, 1.2f, 0.2f, 0, E_QUAD_OUT, G_NONE);
    tween_to(&hero.scale.y, 0.75f, 0.2f, 0, E_QUAD_OUT, G_NONE);
    tween_to(&hero.scale.z, 1, 0.2f, 0, E_QUAD_OUT, G_NONE);
}

static void player_move_on_entity(void) {
    if (!hero.riding_on) return;
    hero.pos.x += hero.riding_on->speed;
    hero.initial.x = hero.pos.x;
    hero.target.x = hero.pos.x; /* initialPosition aliases targetPosition after a hop */
}

static void player_move_on_car(void) {
    if (!hero.hit_by) return;
    hero.pos.x += hero.hit_by->speed;
    if (hero.has_initial) { hero.initial.x = hero.hit_by->pos.x; hero.target.x = hero.initial.x; }
}

static void player_get_run_over(const Row *road) {
    hero.pos.y = row_top(road) - 0.05f;
    tween_to(&hero.scale.y, 0.05f, 0.2f, 0, E_QUAD_OUT, G_NONE);
    tween_to(&hero.scale.x, 1.7f, 0.2f, 0, E_QUAD_OUT, G_NONE);
    tween_to(&hero.scale.z, 1.7f, 0.2f, 0, E_QUAD_OUT, G_NONE);
    tween_to(&hero.rot.y, frand() * PI_F - PI_2, 0.2f, 0, E_QUAD_OUT, G_NONE);
}

static void player_get_hit(const Row *road, Entity *car, float dur) {
    int forward = hero.pos.z - js_round(hero.pos.z) > 0;
    hero.hit_by = car;
    hero.pos.z = road->z + (forward ? 0.52f : -0.52f);
    tween_to(&hero.scale.y, 1.5f, dur, 0, E_QUAD_OUT, G_NONE);
    tween_to(&hero.scale.z, 0.2f, dur, 0, E_QUAD_OUT, G_NONE);
    tween_to(&hero.rot.z, frand() * PI_F - PI_2, dur, 0, E_QUAD_OUT, G_NONE);
}

static int is_game_ended(void) { return !hero.alive || game_state != GS_PLAYING; }

enum { COLL_NONE, COLL_CAR, COLL_TRAIN };

static void on_collide(float speed, int water, int collision) {
    if (is_game_ended()) return;
    hero.alive = 0;
    player_stop_idle();
    if (collision == COLL_CAR) {
        audio_play(frand() < 0.5f ? SND_CAR_HIT0 : SND_CAR_HIT1, 1);
        play_death_sound();
    } else if (collision == COLL_TRAIN) {
        audio_play(SND_TRAIN_DIE, 1);
        play_death_sound();
    }
    if (water) run_water(hero.pos);
    else run_feathers(hero.pos, speed);
    rumble();
    game_over();
}

/* ============================================================ rows */

static void grass_add_obstacle(Row *r, int x) {
    int i;
    for (i = 0; i < r->nclear; i++) if (r->clear[i] == x) return;
    if (r->nobs >= 16) return;
    r->obs_mesh[r->nobs] = (frand() < 0.4f) ? boulder_meshes[(int)(frand() * 2)] : tree_meshes[(int)(frand() * 4)];
    r->obs_x[r->nobs] = x;
    r->nobs++;
}

enum { FILL_EMPTY, FILL_SOLID, FILL_RANDOM };

static void grass_generate(Row *r, int fill, const int *clear, int nclear) {
    int x, rc = 0, count;
    r->nobs = 0;
    r->nclear = nclear > 16 ? 16 : nclear;
    if (nclear) memcpy(r->clear, clear, sizeof(int) * r->nclear);
    count = (int)js_round(frand() * 2) + 1;
    for (x = -3; x < 12; x++) {
        int _x = x - 4;
        if (fill == FILL_SOLID) { grass_add_obstacle(r, _x); continue; }
        if (x >= 9 || x <= -1) { grass_add_obstacle(r, _x); continue; }   /* walls */
        if (fill == FILL_RANDOM && rc < count) {
            if (_x != 0 && frand() > 0.6f) { grass_add_obstacle(r, _x); rc++; }
        }
    }
}

static int grass_blocked(const Row *r, int x) {
    int i;
    for (i = 0; i < r->nobs; i++) if (r->obs_x[i] == x) return 1;
    return 0;
}

static float mesh_width_x(const Mesh *m) { return js_round(m->max[0] - m->min[0]); }
static float mesh_width_z(const Mesh *m) { return js_round(m->max[2] - m->min[2]); }

static void road_car_gen(Row *r) {
    float speed = frand() * 0.06f + 0.02f;
    int n = (int)(frand() * 2) + 1, i;
    int dir = frand() > 0.5f ? -1 : 1;
    float xpos = -6.0f * dir;
    r->nents = n;
    for (i = 0; i < n; i++) {
        Entity *e = &r->ents[i];
        memset(e, 0, sizeof(*e));
        e->mesh = car_meshes[(int)(frand() * 8)];
        e->width = mesh_width_z(e->mesh);
        e->collision = HERO_WIDTH / 2 + e->width / 2 - 0.1f;
        e->dir = dir;
        e->pos.x = xpos; e->pos.y = 0.25f; e->pos.z = 0;
        e->speed = speed * dir;
        e->rot_y = PI_2 * dir;
        xpos -= (frand() * 3 + 5) * dir;
    }
}

static void foam_setup_part(FoamPart *p, int i, int dir) {
    p->x = (frand() * 0.2f - 0.1f);
    p->z = (0.6f / 6) * i + 0.2f;
    p->s = 0.01f;
    p->rot_y = frand() * 0.6f - 0.3f;
    p->phase = 0;
    p->t = 0;
    p->delay = (0.4f + 1.2f) * 0.2f * i;
    p->x_from = p->x;
    p->x_to = p->x + (frand() * 0.8f + 0.2f) * dir;
}

static void foam_update(Row *r) {
    int side, i;
    for (side = 0; side < 2; side++) {
        int dir = side == 0 ? 1 : -1;
        for (i = 0; i < 6; i++) {
            FoamPart *p = &r->foam[side][i];
            p->t += DT;
            if (p->phase == 0) {
                if (p->t >= p->delay) { p->phase = 1; p->t -= p->delay; }
            } else if (p->phase == 1) {
                float u = p->t / 0.4f;
                if (u >= 1) { u = 1; p->phase = 2; p->t = 0; }
                p->s = 0.01f + (1 - 0.01f) * ease_fn(E_BOUNCE_OUT, u);
            } else {
                float u = p->t / 1.2f;
                if (u >= 1) { foam_setup_part(p, i, dir); continue; }
                p->s = 1 + (0.01f - 1) * ease_fn(E_CUBIC_IN, u);
                p->x = p->x_from + (p->x_to - p->x_from) * ease_fn(E_QUAD_OUT, u);
            }
        }
    }
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void water_generate(Row *r, const int *clear, int nclear) {
    int i;
    r->nents = 0;
    r->nlily = 0;
    if (((int)r->z) % 2 == 0) {
        /* generateStatic: lily pads */
        int n = (int)(frand() * 2) + 2;
        int pos[4];
        int xpos = (int)floorf(frand() * 2 - 4);
        for (i = 0; i < n; i++) { pos[i] = xpos; xpos += (int)floorf(frand() * 2 + 2); }
        if (nclear > 0) {
            int ok = 0, j;
            for (i = 0; i < n; i++) for (j = 0; j < nclear; j++) if (pos[i] == clear[j]) ok = 1;
            if (!ok) {
                pos[0] = clear[(int)(frand() * nclear)];
                qsort(pos, n, sizeof(int), cmp_int);
            }
        }
        for (i = 0; i < n; i++) {
            Entity *e = &r->ents[i];
            memset(e, 0, sizeof(*e));
            e->mesh = &mesh_lily_pad;
            e->top = 0.2f; e->min = 0.01f; e->mid = 0.125f; e->dir = 0;
            e->width = mesh_width_x(e->mesh);
            e->collision = HERO_WIDTH / 2 + e->width / 2 - 0.1f;
            e->pos.x = (float)pos[i]; e->pos.y = 0.125f;
            e->speed = 0;
            e->lily_amp = frand() * 1.5f + 0.5f;
            e->lily_period = frand() * 2 + 2;
            e->lily_t = 0;
            r->lily_pos[i] = pos[i];
        }
        r->nents = n;
        r->nlily = n;
    } else {
        /* generateDynamic: logs */
        float speed = frand() * 0.05f + 0.02f;
        int n = (int)(frand() * 2) + 2;
        int dir = frand() > 0.5f ? -1 : 1;
        float xpos = -6.0f * dir;
        for (i = 0; i < n; i++) {
            Entity *e = &r->ents[i];
            memset(e, 0, sizeof(*e));
            e->mesh = log_meshes[(int)(frand() * 4)];
            e->top = 0.3f; e->min = -0.3f; e->mid = -0.1f; e->dir = dir;
            e->width = mesh_width_x(e->mesh);
            e->collision = HERO_WIDTH / 2 + e->width / 2 - 0.1f;
            e->pos.x = xpos; e->pos.y = -0.1f;
            e->speed = speed * dir;
            xpos -= (frand() * 3 + 5) * dir;
        }
        r->nents = n;
    }
}

static void rail_generate(Row *r) {
    int mids = frand() < 0.5f ? 2 : 3;
    float offset = js_round(mesh_train_front.max[0] - mesh_train_front.min[0]);
    int i;
    for (i = 0; i < mids; i++) offset += js_round(mesh_train_middle.max[0] - mesh_train_middle.min[0]);
    r->train_mids = mids;
    memset(&r->train, 0, sizeof(r->train));
    r->train.speed = 0.8f;
    r->train.pos.y = GROUND_LEVEL;
    r->train.pos.z = 0.1f;
    /* FIX: collide against the real extent (front car is centred on the origin) */
    r->train_minx = mesh_train_front.min[0];
    r->train_maxx = offset + mesh_train_back.max[0];
    /* start somewhere along its loop instead of always mid-screen */
    r->train.pos.x = -110 + frand() * 220;
    r->ringing = 0; r->light_off = 1; r->light_a = r->light_b = 0;
}

static void rail_start_ringing(Row *r) {
    r->ringing = 1;
    r->ring_count = 0;
    r->ring_timer = 0;
    r->light_off = 0;
    r->light_b = r->light_a;
    r->light_a = !r->light_a;
    r->ring_count = 1;
}

static void rail_update_light(Row *r) {
    if (!r->ringing) return;
    r->ring_timer += DT;
    if (r->ring_timer >= 0.2f) {
        r->ring_timer -= 0.2f;
        if (r->ring_count < 15) {
            r->light_off = 0;
            r->ring_count++;
            r->light_b = r->light_a;
            r->light_a = !r->light_a;
        } else {
            r->ringing = 0;
            r->ring_count = 0;
            r->light_off = 1;
            r->light_a = r->light_b = 0;
        }
    }
}

static void road_update(Row *r) {
    int i;
    if (!r->active) return;
    for (i = 0; i < r->nents; i++) {
        Entity *car = &r->ents[i];
        const float offset = 11;
        car->pos.x += car->speed;
        if (car->pos.x > offset && car->speed > 0) {
            car->pos.x = -offset;
            if (hero.hit_by == car) hero.hit_by = 0;
        } else if (car->pos.x < -offset && car->speed < 0) {
            car->pos.x = offset;
            if (hero.hit_by == car) hero.hit_by = 0;
        } else if (js_round(hero.pos.z) == r->z && hero.alive) {
            if (hero.pos.x < car->pos.x + car->collision && hero.pos.x > car->pos.x - car->collision) {
                if (hero.moving && fabsf(hero.pos.z - js_round(hero.pos.z)) > 0.1f) player_get_hit(r, car, 0.15f);
                else player_get_run_over(r);
                on_collide(car->speed, 0, COLL_CAR);
            }
        }
    }
}

static void rail_update(Row *r) {
    Entity *train = &r->train;
    const float offset = 22 * 5;
    rail_update_light(r);
    if (!r->active) return;
    train->pos.x += train->speed;
    if (train->pos.x > offset && train->speed > 0) {
        float dist = fabsf(r->z - hero.pos.z);
        train->pos.x = -offset;
        rail_start_ringing(r);
        if (dist < 14) audio_play(SND_TRAIN_PASS, dist < 4 ? 1.0f : 4.0f / dist);
    } else if (!hero.moving) {
        if (js_round(hero.pos.z) == r->z && hero.alive) {
            float lo = train->pos.x + r->train_minx - (HERO_WIDTH / 2 - 0.1f);
            float hi = train->pos.x + r->train_maxx + (HERO_WIDTH / 2 - 0.1f);
            if (hero.pos.x < hi && hero.pos.x > lo) {
                hero.pos.y = GROUND_LEVEL;
                tween_to(&hero.scale.y, 0.2f, 0.3f, 0, E_QUAD_OUT, G_NONE);
                tween_to(&hero.scale.x, 1.5f, 0.3f, 0, E_QUAD_OUT, G_NONE);
                tween_to(&hero.rot.y, frand() * PI_F - PI_2, 0.3f, 0, E_QUAD_OUT, G_NONE);
                on_collide(train->speed, 0, COLL_TRAIN);
            }
        }
    }
}

static Entity *water_collision_entity(Row *r, float x) {
    int i;
    for (i = 0; i < r->nents; i++) {
        Entity *e = &r->ents[i];
        if (x < e->pos.x + e->collision && x > e->pos.x - e->collision) return e;
    }
    return 0;
}

static float water_sunken_y(const Row *r) { return sinf(r->sine_count) * 0.08f - 0.2f; }

static void water_bounce(Entity *e) {
    const float timing = 0.2f;
    tween_to(&e->pos.y, e->min, timing * 0.9f, 0, E_QUAD_OUT, G_NONE);
    tween_to(&e->pos.y, e->mid, timing, timing, E_QUAD_OUT, G_NONE);
    tween_to(&hero.pos.y, e->top + e->min, timing * 0.9f, 0, E_QUAD_OUT, G_NONE);
    tween_to(&hero.pos.y, e->top + e->mid, timing, timing, E_QUAD_OUT, G_NONE);
}

static void water_update(Row *r) {
    int i;
    foam_update(r);
    for (i = 0; i < r->nents; i++) {
        Entity *e = &r->ents[i];
        if (e->lily_period > 0) {
            /* yoyo rotation tween, Power2.easeInOut */
            float ph, u;
            e->lily_t += DT;
            ph = fmodf(e->lily_t, e->lily_period * 2);
            u = ph < e->lily_period ? ph / e->lily_period : 2 - ph / e->lily_period;
            e->rot_y = e->lily_amp * ease_fn(E_CUBIC_INOUT, u);
        }
    }
    if (!r->active) return;
    for (i = 0; i < r->nents; i++) {
        Entity *e = &r->ents[i];
        const float offset = 11;
        e->pos.x += e->speed;
        if (e->pos.x > offset && e->speed > 0) e->pos.x = -offset;
        else if (e->pos.x < -offset && e->speed < 0) e->pos.x = offset;
    }
    if (!hero.moving && !hero.riding_on) {
        if (js_round(hero.pos.z) == r->z && hero.alive) {
            Entity *e = water_collision_entity(r, hero.pos.x);
            if (e) {
                hero.riding_on = e;
                hero.riding_on_offset = hero.pos.x - e->pos.x;
                water_bounce(e);
            }
        }
        /* shouldCheckHazardCollision */
        if (js_round(hero.pos.z) == r->z && !hero.moving && !hero.riding_on) {
            if (hero.alive) {
                on_collide(0, 1, COLL_NONE);
            } else {
                hero.pos.y = water_sunken_y(r);
                r->sine_count += PI_F / 50;
                hero.rot.y += 0.01f;
                if (r->nents) hero.pos.x += r->ents[0].speed;
            }
        }
    }
}

/* ============================================================ map */

static void map_reset(void) {
    grass_count = water_count = road_count = rail_count = 0;
    row_count = 0;
    memset(floor_map, 0, sizeof(floor_map));
}

static int map_row_to_obstacle(void) {
    if (row_count < 5) return FILL_SOLID;
    if (row_count < 10) return FILL_EMPTY;
    return FILL_RANDOM;
}

static void map_new_row(int kind) {
    Row *prev;
    if (grass_count == MAX_ROWS) grass_count = 0;
    if (road_count == MAX_ROWS) road_count = 0;
    if (water_count == MAX_ROWS) water_count = 0;
    if (rail_count == MAX_ROWS) rail_count = 0;
    if (row_count < 10) kind = ROW_GRASS;
    if (kind < 0) {
        int k = (int)(frand() * 3);
        kind = k == 0 ? ROW_GRASS : (k == 1 ? ROW_ROAD : ROW_WATER);
    }
    prev = get_row(row_count - 1);

    if (kind == ROW_GRASS) {
        Row *r = &grass_rows[grass_count++];
        int clear[4], nclear = 0, i;
        r->type = ROW_GRASS;
        r->z = (float)row_count;
        r->active = 1;
        if (prev && prev->type == ROW_WATER) {
            for (i = 0; i < prev->nlily; i++) clear[nclear++] = prev->lily_pos[i];
        }
        grass_generate(r, map_row_to_obstacle(), clear, nclear);
        set_row(row_count, r);
    } else if (kind == ROW_ROAD) {
        if ((int)(frand() * 4) == 0) {
            Row *r = &rail_rows[rail_count++];
            r->type = ROW_RAIL;
            r->z = (float)row_count;
            r->active = 1;
            rail_generate(r);
            set_row(row_count, r);
        } else {
            Row *r = &road_rows[road_count++];
            r->type = ROW_ROAD;
            r->z = (float)row_count;
            r->first_lane = !(prev && prev->type == ROW_ROAD);
            r->active = 1;
            road_car_gen(r);
            set_row(row_count, r);
        }
    } else {
        Row *r = &water_rows[water_count++];
        int clear[16], nclear = 0, x;
        r->type = ROW_WATER;
        r->z = (float)row_count;
        r->active = 1;
        if (prev && prev->type == ROW_GRASS) {
            for (x = -4; x <= 4; x++) if (!grass_blocked(prev, x)) clear[nclear++] = x;
        }
        water_generate(r, clear, nclear);
        set_row(row_count, r);
    }
    row_count++;
}

static void map_init(void) {
    int i, s, k;
    for (i = 0; i < MAX_ROWS; i++) {
        grass_rows[i].type = ROW_GRASS; grass_rows[i].z = MAP_OFFSET; grass_rows[i].nobs = 0;
        water_rows[i].type = ROW_WATER; water_rows[i].z = MAP_OFFSET; water_rows[i].active = 0; water_rows[i].nents = 0;
        road_rows[i].type = ROW_ROAD; road_rows[i].z = MAP_OFFSET; road_rows[i].active = 0; road_rows[i].nents = 0;
        rail_rows[i].type = ROW_RAIL; rail_rows[i].z = MAP_OFFSET; rail_rows[i].active = 0;
        rail_generate(&rail_rows[i]);
        for (s = 0; s < 2; s++) for (k = 0; k < 6; k++) foam_setup_part(&water_rows[i].foam[s][k], k, s == 0 ? 1 : -1);
    }
    {
        Row *r = &grass_rows[grass_count++];
        r->z = (float)row_count;
        grass_generate(r, map_row_to_obstacle(), 0, 0);
        set_row(row_count, r);
        row_count++;
    }
    for (i = 0; i < MAX_ROWS + 3; i++) map_new_row(-1);
}

static int tree_collision(float x, float z) {
    Row *r = get_row((int)z);
    if (r && r->type == ROW_GRASS) return grass_blocked(r, (int)x);
    return 0;
}

/* ============================================================ engine */

static void game_over(void) {
    hero.moving = 0;
    player_stop_animations();
    if (game_state == GS_PLAYING) {
        game_state = GS_GAMEOVER;
        state_time = 0;
        new_highscore = 0;
        if (score > highscore) { highscore = score; new_highscore = 1; save_settings(); }
    }
}

static void engine_init(void) {
    score = 0;
    tween_kill_all();
    player_reset();
    memset(feathers, 0, sizeof(feathers));
    memset(water_parts, 0, sizeof(water_parts));
    cam_count = 0;
    queued_dir = -1;
    /* FIX: reset the camera too */
    world_pos.x = world_pos.y = world_pos.z = 0;
    scene_pos.x = scene_pos.y = scene_pos.z = 0;
    map_reset();
    player_idle_start();
    map_init();
}

static void forward_scene(void) {
    float target_x;
    world_pos.z -= (hero.pos.z - STARTING_ROW + world_pos.z) * CAMERA_EASING;
    target_x = -hero.pos.x;
    if (target_x < -3) target_x = -3;
    if (target_x > 2) target_x = 2;
    world_pos.x += (target_x - world_pos.x) * CAMERA_EASING;
    if (-world_pos.z - cam_count > 1.0f) {
        cam_count = -world_pos.z;
        map_new_row(-1);
    }
    /* FIX: never let the chicken get near the edge of the generated map,
       even if it outpaces the camera */
    while (row_count < (int)hero.pos.z + 16) map_new_row(-1);
}

static void check_out_of_frame(void) {
    if (is_game_ended()) return;
    if (hero.pos.x < -5 || hero.pos.x > 5) {
        rumble();
        game_over();
        play_death_sound();
    }
}

static void engine_tick(void) {
    int i;
    for (i = 0; i < MAX_ROWS; i++) rail_update(&rail_rows[i]);
    for (i = 0; i < MAX_ROWS; i++) road_update(&road_rows[i]);
    for (i = 0; i < MAX_ROWS; i++) water_update(&water_rows[i]);
    if (!hero.moving) {
        player_move_on_entity();
        player_move_on_car();
        check_out_of_frame();
    }
    forward_scene();
}

enum { SWIPE_UP, SWIPE_DOWN, SWIPE_LEFT, SWIPE_RIGHT };

static void move_with_direction(int dir) {
    int vz = 0;
    Row *target_row;
    float final_y;
    if (is_game_ended()) return;

    hero.riding_on = 0;
    if (!hero.has_initial) {
        hero.initial = hero.pos;
        hero.target = hero.initial;
        hero.has_initial = 1;
    }
    player_skip_pending_movement();
    hero.target_rot = normalize_angle(hero.rot.y);

    switch (dir) {
    case SWIPE_LEFT:
        hero.target_rot = PI_2;
        hero.target = hero.initial; hero.target.x += 1;
        hero.moving = 1;
        break;
    case SWIPE_RIGHT:
        hero.target_rot = -PI_2;
        hero.target = hero.initial; hero.target.x -= 1;
        hero.moving = 1;
        break;
    case SWIPE_UP: {
        Row *r = get_row((int)js_round(hero.initial.z));
        hero.target_rot = 0;
        if (r && r->type == ROW_ROAD && frand() < 0.5f) audio_play(SND_CAR_HORN, 0.8f);
        vz = 1;
        hero.target = hero.initial; hero.target.z += 1;
        hero.target.x = js_round(hero.target.x);
        hero.moving = 1;
        break;
    }
    case SWIPE_DOWN:
        hero.target_rot = PI_F;
        vz = -1;
        hero.target = hero.initial; hero.target.z -= 1;
        hero.target.x = js_round(hero.target.x);
        hero.moving = 1;
        break;
    }

    if (tree_collision(hero.target.x, hero.target.z)) {
        hero.target = hero.initial;
        hero.moving = 0;
    }

    target_row = get_row((int)js_round(hero.initial.z) + vz);
    final_y = target_row ? row_top(target_row) : GROUND_LEVEL;
    if (target_row && target_row->type == ROW_WATER) {
        Entity *e = (js_round(hero.target.z) == target_row->z) ? water_collision_entity(target_row, hero.target.x) : 0;
        final_y = e ? e->top + e->mid : water_sunken_y(target_row);
    }

    audio_play(SND_BUCK1 + move_sound_index, 1);
    move_sound_index = (move_sound_index + 1) % 12;

    hero.target.y = final_y;
    player_commit_movement();
}

/* ============================================================ persistence */

static void load_settings(void) {
    char buf[128];
    int top = 0, mute = 0;
    if (plat_load_text("save.txt", buf, sizeof(buf)) > 0) {
        const char *p = strstr(buf, "top=");
        const char *m = strstr(buf, "mute=");
        if (p) top = atoi(p + 4);
        if (m) mute = atoi(m + 5);
    }
    highscore = top > 0 ? top : 0;
    audio_set_muted(mute);
}

static void save_settings(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "top=%d\nmute=%d\n", highscore, audio_muted());
    plat_save_text("save.txt", buf);
}

void game_save(void) { save_settings(); }

/* ============================================================ public API */

void game_init(void) {
    float s, size[3];
    int i;
    r_init();
    audio_mix_init();
    rng_seed((unsigned int)(plat_seconds() * 1000003.0));
    load_settings();
    /* CrossyPlayer.setCharacter: scaleLongestSideToSize(node, 1) + alignMesh(x .5, y 1, z .5) */
    for (i = 0; i < 3; i++) size[i] = mesh_chicken.max[i] - mesh_chicken.min[i];
    s = size[0];
    if (size[1] > s) s = size[1];
    if (size[2] > s) s = size[2];
    hero_pre_scale = 1.0f / s;
    hero_pre_off[0] = -hero_pre_scale * (mesh_chicken.min[0] + size[0] * 0.5f);
    hero_pre_off[1] = -hero_pre_scale * mesh_chicken.min[1];
    hero_pre_off[2] = -hero_pre_scale * (mesh_chicken.min[2] + size[2] * 0.5f);
    engine_init();
    game_state = GS_HOME;
    title_anim = 0;
}

int game_wants_rumble(void) { return rumble_frames > 0; }

static void show_toast(const char *s) {
    snprintf(toast, sizeof(toast), "%s", s);
    toast_time = 1.5f;
}

void game_input(unsigned int pressed, unsigned int released, unsigned int held) {
    const unsigned int dirs = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_CROSS;
    (void)held;
    if (pressed & PAD_TRIANGLE) show_fps = !show_fps;
    if (pressed & PAD_SELECT) {
        audio_set_muted(!audio_muted());
        show_toast(audio_muted() ? "SOUND OFF" : "SOUND ON");
        save_settings();
    }
    switch (game_state) {
    case GS_HOME:
        /* HomeScreen: space/up (keyup) starts the game */
        if (released & (PAD_CROSS | PAD_UP | PAD_START)) {
            audio_play(SND_POP1, 1);
            game_state = GS_PLAYING;
            state_time = 0;
        }
        break;
    case GS_PLAYING:
        if (pressed & PAD_START) {
            paused_from = GS_PLAYING;
            game_state = GS_PAUSED;
            audio_play(SND_POP2, 1);
            break;
        }
        /* GestureView: touch-down squashes the chicken, release hops */
        if ((pressed & dirs) && !is_game_ended()) player_posie();
        {
            /* FIX: a hop pressed mid-air is buffered and starts on landing
               (after the landing has been checked for water/trees), instead
               of snapping to the target and chaining hops forever. */
            int dir = -1;
            if (released & (PAD_UP | PAD_CROSS)) dir = SWIPE_UP;
            else if (released & PAD_DOWN) dir = SWIPE_DOWN;
            else if (released & PAD_LEFT) dir = SWIPE_LEFT;
            else if (released & PAD_RIGHT) dir = SWIPE_RIGHT;
            if (dir >= 0) {
                if (hero.moving) queued_dir = dir;
                else move_with_direction(dir);
            }
        }
        break;
    case GS_PAUSED:
        if (pressed & (PAD_START | PAD_CROSS)) {
            game_state = paused_from;
            audio_play(SND_POP1, 1);
        } else if (pressed & PAD_CIRCLE) {
            engine_init();
            game_state = GS_HOME;
            title_anim = 0;
            audio_play(SND_POP2, 1);
        }
        break;
    case GS_GAMEOVER:
        if (state_time > 0.8f && (pressed & (PAD_CROSS | PAD_START))) {
            audio_play(SND_POP1, 1);
            engine_init();
            game_state = GS_PLAYING;
            state_time = 0;
        } else if (state_time > 0.8f && (pressed & PAD_CIRCLE)) {
            engine_init();
            game_state = GS_HOME;
            title_anim = 0;
        }
        break;
    }
    prev_dir_held = held;
}

void game_step(void) {
    if (toast_time > 0) toast_time -= DT;
    if (rumble_frames > 0) rumble_frames--;
    if (game_state == GS_PAUSED) return;
    state_time += DT;
    if (title_anim < 1) title_anim += DT / 0.8f;
    if (game_state == GS_GAMEOVER && state_time - DT < 0.6f && state_time >= 0.6f) audio_play(SND_BANNER, 1);
    tweens_update(DT);
    player_idle_update();
    particles_update();
    engine_tick();
    /* run a buffered hop once the landing has been processed */
    if (queued_dir >= 0 && !hero.moving) {
        int dir = queued_dir;
        queued_dir = -1;
        if (game_state == GS_PLAYING && hero.alive) move_with_direction(dir);
    }
}

/* ============================================================ rendering */

static V3 world_offset(void) {
    V3 o;
    o.x = world_pos.x + scene_pos.x;
    o.y = world_pos.y + scene_pos.y;
    o.z = world_pos.z - STARTING_ROW + scene_pos.z;
    return o;
}

static void set_xf(Xform *x, V3 off, float px, float py, float pz, float rot_y) {
    r_xform_identity(x);
    x->pos[0] = px + off.x; x->pos[1] = py + off.y; x->pos[2] = pz + off.z;
    x->rot[1] = rot_y;
}

static const Mesh *floor_mesh(const Row *r) {
    switch (r->type) {
    case ROW_GRASS: return (((int)r->z) & 1) ? &mesh_grass_dark : &mesh_grass_light;
    case ROW_ROAD: return r->first_lane ? &mesh_road_blank : &mesh_road_stripes;
    case ROW_WATER: return &mesh_river;
    default: return &mesh_railroad;
    }
}

static int row_visible(const Row *r, V3 off) {
    float z = r->z + off.z;
    return z > -9 && z < 16;
}

static void draw_world(int ctx) {
    Row *all_rows[MAX_ROWS * 4];
    V3 off = world_offset();
    Xform x;
    int i, j, n = 0;
    float hero_plane = GROUND_LEVEL - 0.025f;

    for (i = 0; i < MAX_ROWS; i++) {
        all_rows[n++] = &grass_rows[i];
        all_rows[n++] = &road_rows[i];
        all_rows[n++] = &water_rows[i];
        all_rows[n++] = &rail_rows[i];
    }

    /* 1. ground strips */
    for (i = 0; i < n; i++) {
        Row *r = all_rows[i];
        if (!row_visible(r, off)) continue;
        set_xf(&x, off, 0, 0, r->z, 0);
        r_draw_mesh(ctx, floor_mesh(r), &x, 0);
    }

    /* 2. shadows on the ground */
    r_shadow_pass_begin(ctx);
    for (i = 0; i < n; i++) {
        Row *r = all_rows[i];
        float py = row_floor_y(r);
        if (!row_visible(r, off)) continue;
        if (r->type == ROW_GRASS) {
            for (j = 0; j < r->nobs; j++) {
                set_xf(&x, off, (float)r->obs_x[j], GROUND_LEVEL, r->z, 0);
                if (r_mesh_on_screen(r->obs_mesh[j], &x)) r_draw_shadow(ctx, r->obs_mesh[j], &x, py + off.y);
            }
        } else if (r->type == ROW_ROAD || r->type == ROW_WATER) {
            for (j = 0; j < r->nents; j++) {
                Entity *e = &r->ents[j];
                set_xf(&x, off, e->pos.x, e->pos.y, r->z + e->pos.z, e->rot_y);
                if (r_mesh_on_screen(e->mesh, &x)) r_draw_shadow(ctx, e->mesh, &x, py + off.y);
            }
        } else if (r->type == ROW_RAIL && r->active) {
            float ox = r->train.pos.x;
            if (ox + r->train_maxx + off.x > -12 && ox + r->train_minx + off.x < 12) {
                float cx = 0;
                set_xf(&x, off, ox, r->train.pos.y, r->z + r->train.pos.z, 0);
                r_draw_shadow(ctx, &mesh_train_front, &x, py + off.y);
                cx = js_round(mesh_train_front.max[0] - mesh_train_front.min[0]);
                for (j = 0; j < r->train_mids; j++) {
                    set_xf(&x, off, ox + cx, r->train.pos.y, r->z + r->train.pos.z, 0);
                    r_draw_shadow(ctx, &mesh_train_middle, &x, py + off.y);
                    cx += js_round(mesh_train_middle.max[0] - mesh_train_middle.min[0]);
                }
                set_xf(&x, off, ox + cx, r->train.pos.y, r->z + r->train.pos.z, 0);
                r_draw_shadow(ctx, &mesh_train_back, &x, py + off.y);
            }
        }
    }
    {
        Row *under = get_row((int)js_round(hero.pos.z));
        if (under) hero_plane = row_floor_y(under);
        r_xform_identity(&x);
        x.pos[0] = hero.pos.x + off.x; x.pos[1] = hero.pos.y + off.y; x.pos[2] = hero.pos.z + off.z;
        x.rot[0] = hero.rot.x; x.rot[1] = hero.rot.y; x.rot[2] = hero.rot.z;
        x.scale[0] = hero.scale.x; x.scale[1] = hero.scale.y; x.scale[2] = hero.scale.z;
        x.pre_scale = hero_pre_scale;
        memcpy(x.pre_off, hero_pre_off, sizeof(x.pre_off));
        if (hero.pos.y > hero_plane - 0.05f) r_draw_shadow(ctx, &mesh_chicken, &x, hero_plane + off.y);
    }

    /* 3. objects */
    for (i = 0; i < n; i++) {
        Row *r = all_rows[i];
        if (!row_visible(r, off)) continue;
        if (r->type == ROW_GRASS) {
            for (j = 0; j < r->nobs; j++) {
                set_xf(&x, off, (float)r->obs_x[j], GROUND_LEVEL, r->z, 0);
                r_draw_mesh(ctx, r->obs_mesh[j], &x, 0);
            }
        } else if (r->type == ROW_ROAD || r->type == ROW_WATER) {
            for (j = 0; j < r->nents; j++) {
                Entity *e = &r->ents[j];
                set_xf(&x, off, e->pos.x, e->pos.y, r->z + e->pos.z, e->rot_y);
                r_draw_mesh(ctx, e->mesh, &x, 0);
            }
            if (r->type == ROW_WATER) {
                int s, k;
                for (s = 0; s < 2; s++) {
                    float bx = s == 0 ? 4.5f : -4.5f;
                    for (k = 0; k < 6; k++) {
                        FoamPart *p = &r->foam[s][k];
                        float c[3];
                        if (p->phase == 0 || p->s < 0.02f) continue;
                        c[0] = bx + p->x + off.x; c[1] = 0.2f + off.y; c[2] = r->z - 0.5f + p->z + off.z;
                        r_draw_quad_flat(ctx, c, 0.6f * p->s, p->rot_y, 0xFFFFFF);
                    }
                }
            }
        } else if (r->type == ROW_RAIL) {
            const Mesh *lm = r->light_off ? &mesh_light_off : (r->light_a ? &mesh_light_on0 : &mesh_light_on1);
            set_xf(&x, off, 0, 0, r->z - 0.5f, PI_F);
            r_draw_mesh(ctx, lm, &x, 0);
            if (r->active) {
                float ox = r->train.pos.x, cx;
                if (ox + r->train_maxx + off.x > -12 && ox + r->train_minx + off.x < 12) {
                    set_xf(&x, off, ox, r->train.pos.y, r->z + r->train.pos.z, 0);
                    r_draw_mesh(ctx, &mesh_train_front, &x, 0);
                    cx = js_round(mesh_train_front.max[0] - mesh_train_front.min[0]);
                    for (j = 0; j < r->train_mids; j++) {
                        set_xf(&x, off, ox + cx, r->train.pos.y, r->z + r->train.pos.z, 0);
                        r_draw_mesh(ctx, &mesh_train_middle, &x, 0);
                        cx += js_round(mesh_train_middle.max[0] - mesh_train_middle.min[0]);
                    }
                    set_xf(&x, off, ox + cx, r->train.pos.y, r->z + r->train.pos.z, 0);
                    r_draw_mesh(ctx, &mesh_train_back, &x, 0);
                }
            }
        }
    }

    /* hero */
    r_xform_identity(&x);
    x.pos[0] = hero.pos.x + off.x; x.pos[1] = hero.pos.y + off.y; x.pos[2] = hero.pos.z + off.z;
    x.rot[0] = hero.rot.x; x.rot[1] = hero.rot.y; x.rot[2] = hero.rot.z;
    x.scale[0] = hero.scale.x; x.scale[1] = hero.scale.y; x.scale[2] = hero.scale.z;
    x.pre_scale = hero_pre_scale;
    memcpy(x.pre_off, hero_pre_off, sizeof(x.pre_off));
    r_draw_mesh(ctx, &mesh_chicken, &x, 0);

    /* particles */
    for (i = 0; i < 20; i++) {
        Particle *p = &feathers[i];
        float c[3], s[3], rr[3];
        if (!p->visible) continue;
        c[0] = feathers_origin.x + p->pos.x + off.x;
        c[1] = feathers_origin.y + p->pos.y + off.y;
        c[2] = feathers_origin.z + p->pos.z + off.z;
        s[0] = 0.1f * p->scale.x; s[1] = 0.1f * p->scale.y; s[2] = 0.02f * p->scale.z;
        rr[0] = p->rot.x; rr[1] = p->rot.y; rr[2] = p->rot.z;
        r_draw_box(ctx, c, s, rr, 0xFFFFFF, 0, 0);
    }
    for (i = 0; i < 15; i++) {
        Particle *p = &water_parts[i];
        float c[3], s[3], rr[3] = { 0, 0, 0 };
        if (!p->visible) continue;
        c[0] = water_origin.x + p->pos.x + off.x;
        c[1] = water_origin.y + p->pos.y + off.y;
        c[2] = water_origin.z + p->pos.z + off.z;
        s[0] = 0.2f * p->scale.x; s[1] = 0.3f * p->scale.y; s[2] = 0.2f * p->scale.z;
        r_draw_box(ctx, c, s, rr, 0x71D7FF, 0, 0);
    }
}

static void draw_ui(void) {
    char buf[48];
    if (game_state == GS_HOME) {
        float a = title_anim > 1 ? 1 : title_anim;
        float e = 1 - (1 - a) * (1 - a);
        int tx = (int)(-RW + (RW + 0) * e);
        int ty = (int)(-100 + 100 * e);
        const char *l1 = "BOUNCY", *l2 = "BACON";
        int w1 = r_text_width(l1, 9), w2 = r_text_width(l2, 9);
        r_text_3d(tx + (RW - w1) / 2 - 20, ty + 40, l1, 9, 0xFFFFFF, 0x000000, 8);
        r_text_3d(tx + (RW - w2) / 2 + 20, ty + 110, l2, 9, 0xFFFFFF, 0x000000, 8);
        if (((int)(state_time * 2)) % 2 == 0) r_text_centered(250, "PRESS X TO PLAY", 3, 0xFFFFFF, 2);
        r_text_centered(300, "D-PAD / LEFT STICK: HOP", 2, 0xFFFFFF, 2);
        r_text_centered(320, "START: PAUSE   SELECT: SOUND", 2, 0xFFFFFF, 2);
        if (highscore > 0) {
            snprintf(buf, sizeof(buf), "TOP %d", highscore);
            r_text(14, 14, buf, 3, 0xFFFF00, 2);
        }
    } else {
        snprintf(buf, sizeof(buf), "%d", score);
        r_text(14, 14, buf, 6, 0xFFFFFF, 3);
        if (highscore > 0) {
            snprintf(buf, sizeof(buf), "TOP %d", highscore);
            r_text(14, 66, buf, 3, 0xFFFF00, 2);
        }
    }
    if (game_state == GS_PAUSED) {
        r_shade_rect(0, 0, RW, RH, 120, 0x000000);
        r_text_centered(120, "PAUSED", 7, 0xFFFFFF, 3);
        r_text_centered(200, "X / START: RESUME", 3, 0xFFFFFF, 2);
        r_text_centered(230, "O: QUIT TO TITLE", 3, 0xFFFFFF, 2);
    }
    if (game_state == GS_GAMEOVER && state_time > 0.6f) {
        float a = (state_time - 0.6f) / 0.3f;
        int y;
        if (a > 1) a = 1;
        y = (int)(-120 + (130 + 120) * ease_fn(E_BOUNCE_OUT, a));
        r_fill_rect(RW / 2 - 170, y, 340, 150, 0x000000);
        r_fill_rect(RW / 2 - 166, y + 4, 332, 142, 0xF5A623);
        r_text_centered(y + 16, "GAME OVER", 5, 0xFFFFFF, 3);
        snprintf(buf, sizeof(buf), "SCORE %d", score);
        r_text_centered(y + 64, buf, 3, 0xFFFFFF, 2);
        if (new_highscore) r_text_centered(y + 90, "NEW TOP SCORE!", 3, 0xFFFF00, 2);
        else { snprintf(buf, sizeof(buf), "TOP %d", highscore); r_text_centered(y + 90, buf, 3, 0xFFFF00, 2); }
        if (state_time > 0.8f) r_text_centered(y + 120, "X: PLAY AGAIN   O: TITLE", 2, 0xFFFFFF, 2);
    }
    if (toast_time > 0) r_text_centered(RH - 40, toast, 3, 0xFFFFFF, 2);
}

/* runs once per screen band, possibly on two hardware threads at once;
   only reads game state */
static void render_band(int ctx) {
    r_clear_band(ctx);
    draw_world(ctx);
}

static double fps_last;
static int fps_frames;
static char fps_text[24] = "";

void game_render(void) {
    r_begin(SCENE_COLOR, VIEW_HALF_HEIGHT, 0, 0);
    plat_parallel(render_band);
    draw_ui();
    /* FPS counter (TRIANGLE toggles) */
    fps_frames++;
    if (plat_seconds() - fps_last >= 1.0) {
        double now = plat_seconds();
        snprintf(fps_text, sizeof(fps_text), "%.0f FPS", fps_frames / (now - fps_last));
        fps_last = now;
        fps_frames = 0;
    }
    if (show_fps) r_text(RW - r_text_width(fps_text, 2) - 10, 10, fps_text, 2, 0xFFFFFF, 1);
}

