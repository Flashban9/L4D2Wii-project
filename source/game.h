/*
 * UNDEAD RUSH - platform independent game logic (no Wii code in here, so it can be tested on a PC).
 * Coordinate system: Y is up, the level runs along -Z, yaw 0 looks toward -Z, positive yaw turns right.
 */
#ifndef GAME_H
#define GAME_H
#include <stdint.h>

#define MAX_WALLS      128
#define MAX_ZOMBIES    64
#define MAX_PARTICLES  96
#define MAX_PICKUPS    16
#define MAX_TRIGGERS   8
#define EYE_HEIGHT     1.6f

enum { ST_PLAYING = 0, ST_DEAD = 1, ST_WON = 2 };
enum { Z_WALKER = 0, Z_RUNNER = 1, Z_BRUTE = 2 };
enum { PK_MEDKIT = 0, PK_AMMO = 1, PK_SMG = 2, PK_SHOTGUN = 3 };

typedef struct { float x0, z0, x1, z1, h; uint8_t r, g, b, glow; } Wall;

typedef struct {
    int   st;                 /* 0 free, 1 alive, 2 dying */
    int   type, alerted, side;
    float x, z, yaw, hp, anim, cd, flash, dying, stuck, groan;
    uint8_t cr, cg, cb;
} Zombie;

typedef struct { float x, y, z, vx, vy, vz, life; uint8_t r, g, b; } Particle;
typedef struct { float x, z; int kind, active; } Pickup;
typedef struct { float z; int fired; } Trigger;

typedef struct {
    const char *name;
    int   mag_max, auto_fire, pellets, reserve_max;   /* reserve_max < 0 = infinite */
    float cd, dmg, spread, reload, noise;
} WeaponDef;

typedef struct {
    float x, z, yaw, pitch, hp;
    int   weapon, owned[3], mag[3], reserve[3];
    float fire_cd, reload_t, hurt, hitmarker, muzzle, recoil;
} Player;

typedef struct {
    int shot, shotgun, hit, headshot, hurt, pickup, reload, roar, zdie, click, groan, horde;
} Events;

typedef struct {
    int    state;
    float  time;
    Player p;
    Wall   walls[MAX_WALLS];   int nwalls;
    Zombie z[MAX_ZOMBIES];
    Particle part[MAX_PARTICLES];
    Pickup pick[MAX_PICKUPS];  int npick;
    Trigger trig[MAX_TRIGGERS]; int ntrig;
    int    kills, headshots;
    char   msg[40]; float msg_t;
    uint32_t rng;
    int    god;
    Events ev;
} Game;

typedef struct {
    float move_f, move_s;            /* -1..1 */
    float yaw_rate, pitch_rate;      /* radians per second */
    float aim_x, aim_y;              /* pointer, -1..1 (x right, y up) */
    float tan_x, tan_y;              /* half-FOV tangents of the camera */
    int   fire, fire_pressed, reload, sprint, weapon_next, weapon_prev;
} Input;

extern const WeaponDef g_weapons[3];

void game_reset(Game *g, uint32_t seed);
void game_update(Game *g, const Input *in, float dt);
void game_clear_events(Game *g);
int  game_blocked(const Game *g, float x, float z, float r);
void game_aim_dir(const Game *g, const Input *in, float out[3]);
int  game_alive_count(const Game *g);

#endif
