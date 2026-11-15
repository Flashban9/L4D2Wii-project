/* UNDEAD RUSH - game logic. Pure C, no console code. */
#include "game.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define PI_F 3.14159265f

const WeaponDef g_weapons[3] = {
    /* name       mag auto pel  reserve  cd     dmg   spread  reload noise */
    { "PISTOL",   15, 0,  1,   -1,     0.26f, 26.f, 0.010f, 1.20f, 26.f },
    { "SMG",      45, 1,  1,  360,     0.07f, 13.f, 0.028f, 1.60f, 32.f },
    { "SHOTGUN",   8, 0,  9,   48,     0.85f, 11.f, 0.070f, 2.30f, 40.f },
};

/* ---------- helpers ---------- */
static float frand(Game *g)
{
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return (float)(g->rng & 0xFFFFFF) / 16777216.0f;
}

static float frange(Game *g, float a, float b) { return a + (b - a) * frand(g); }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static void set_msg(Game *g, const char *text)
{
    snprintf(g->msg, sizeof(g->msg), "%s", text);
    g->msg_t = 3.0f;
}

static void add_wall(Game *g, float x0, float z0, float x1, float z1, float h,
                     int r, int gr, int b, int glow)
{
    if (g->nwalls >= MAX_WALLS) return;
    Wall *w = &g->walls[g->nwalls++];
    w->x0 = x0; w->z0 = z0; w->x1 = x1; w->z1 = z1; w->h = h;
    w->r = (uint8_t)r; w->g = (uint8_t)gr; w->b = (uint8_t)b; w->glow = (uint8_t)glow;
}

int game_blocked(const Game *g, float x, float z, float r)
{
    for (int i = 0; i < g->nwalls; i++) {
        const Wall *w = &g->walls[i];
        float cx = clampf(x, w->x0, w->x1);
        float cz = clampf(z, w->z0, w->z1);
        float dx = x - cx, dz = z - cz;
        if (dx * dx + dz * dz < r * r) return 1;
    }
    return 0;
}

static void move_circle(const Game *g, float *x, float *z, float dx, float dz, float r)
{
    if (!game_blocked(g, *x + dx, *z, r)) *x += dx;
    if (!game_blocked(g, *x, *z + dz, r)) *z += dz;
}

static float ray_box(const float o[3], const float d[3], const float mn[3], const float mx[3])
{
    float tmin = 0.0f, tmax = 1e9f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-8f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return -1.0f;
        } else {
            float t1 = (mn[i] - o[i]) / d[i];
            float t2 = (mx[i] - o[i]) / d[i];
            if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return -1.0f;
        }
    }
    return tmin;
}

void game_aim_dir(const Game *g, const Input *in, float out[3])
{
    float sy = sinf(g->p.yaw), cy = cosf(g->p.yaw);
    float sp = sinf(g->p.pitch), cp = cosf(g->p.pitch);
    float f[3] = { cp * sy, sp, -cp * cy };
    float r[3] = { cy, 0.0f, sy };
    float u[3] = { -sp * sy, cp, sp * cy };
    float ax = in->aim_x * in->tan_x;
    float ay = in->aim_y * in->tan_y;
    float d[3];
    float len = 0.0f;
    for (int i = 0; i < 3; i++) { d[i] = f[i] + r[i] * ax + u[i] * ay; len += d[i] * d[i]; }
    len = sqrtf(len);
    for (int i = 0; i < 3; i++) out[i] = d[i] / len;
}

int game_alive_count(const Game *g)
{
    int n = 0;
    for (int i = 0; i < MAX_ZOMBIES; i++) if (g->z[i].st == 1) n++;
    return n;
}


/* ---------- path finding: a coarse flow field that leads zombies around obstacles ---------- */
#define FG_CELL 0.5f
#define FG_X0   (-14.0f)
#define FG_Z0   (-128.0f)
#define FG_NX   56
#define FG_NZ   272
static uint8_t fg_block[FG_NZ][FG_NX];
static int16_t fg_dist[FG_NZ][FG_NX];
static int16_t fg_qx[FG_NX * FG_NZ], fg_qz[FG_NX * FG_NZ];
static float   fg_timer;

static void fg_build(const Game *g)
{
    for (int j = 0; j < FG_NZ; j++)
        for (int i = 0; i < FG_NX; i++)
            fg_block[j][i] = (uint8_t)game_blocked(g, FG_X0 + (i + 0.5f) * FG_CELL, FG_Z0 + (j + 0.5f) * FG_CELL, 0.55f);
    memset(fg_dist, -1, sizeof(fg_dist));
    fg_timer = 0.0f;
}

static void fg_update(const Game *g)
{
    int cx = (int)((g->p.x - FG_X0) / FG_CELL), cz = (int)((g->p.z - FG_Z0) / FG_CELL);
    if (cx < 0) cx = 0;
    if (cx >= FG_NX) cx = FG_NX - 1;
    if (cz < 0) cz = 0;
    if (cz >= FG_NZ) cz = FG_NZ - 1;
    if (fg_block[cz][cx]) {                         /* player hugs a wall: start from a free neighbour */
        int found = 0;
        for (int oz = -2; oz <= 2 && !found; oz++)
            for (int ox = -2; ox <= 2 && !found; ox++) {
                int nx = cx + ox, nz = cz + oz;
                if (nx < 0 || nz < 0 || nx >= FG_NX || nz >= FG_NZ || fg_block[nz][nx]) continue;
                cx = nx; cz = nz; found = 1;
            }
        if (!found) return;
    }
    memset(fg_dist, -1, sizeof(fg_dist));
    int qh = 0, qt = 0;
    fg_dist[cz][cx] = 0; fg_qx[qt] = (int16_t)cx; fg_qz[qt++] = (int16_t)cz;
    static const int dx[4] = { 1, -1, 0, 0 }, dz[4] = { 0, 0, 1, -1 };
    while (qh < qt) {
        int x = fg_qx[qh], z = fg_qz[qh++];
        for (int k = 0; k < 4; k++) {
            int nx = x + dx[k], nz = z + dz[k];
            if (nx < 0 || nz < 0 || nx >= FG_NX || nz >= FG_NZ) continue;
            if (fg_block[nz][nx] || fg_dist[nz][nx] >= 0) continue;
            fg_dist[nz][nx] = (int16_t)(fg_dist[z][x] + 1);
            fg_qx[qt] = (int16_t)nx; fg_qz[qt++] = (int16_t)nz;
        }
    }
}

/* direction a zombie at (x,z) should walk to get around obstacles; returns 0 if no field is available there */
static int fg_direction(float x, float z, float *dxo, float *dzo)
{
    int cx = (int)((x - FG_X0) / FG_CELL), cz = (int)((z - FG_Z0) / FG_CELL);
    if (cx < 0 || cz < 0 || cx >= FG_NX || cz >= FG_NZ) return 0;
    int cur = fg_dist[cz][cx];
    if (cur < 0) return 0;
    int bx = cx, bz = cz;
    for (int step = 0; step < 3; step++) {          /* look 3 cells ahead for smooth movement */
        int best = fg_dist[bz][bx], nbx = bx, nbz = bz;
        for (int oz = -1; oz <= 1; oz++)
            for (int ox = -1; ox <= 1; ox++) {
                if (!ox && !oz) continue;
                int nx = bx + ox, nz = bz + oz;
                if (nx < 0 || nz < 0 || nx >= FG_NX || nz >= FG_NZ) continue;
                int d = fg_dist[nz][nx];
                if (d < 0 || d >= best) continue;
                if (ox && oz && (fg_block[bz][nx] || fg_block[nz][bx])) continue;   /* no corner cutting */
                best = d; nbx = nx; nbz = nz;
            }
        bx = nbx; bz = nbz;
    }
    float tx = FG_X0 + (bx + 0.5f) * FG_CELL, tz = FG_Z0 + (bz + 0.5f) * FG_CELL;
    float vx = tx - x, vz = tz - z, l = sqrtf(vx * vx + vz * vz);
    if (l < 0.05f) return 0;
    *dxo = vx / l; *dzo = vz / l;
    return 1;
}

/* ---------- particles ---------- */
static void spawn_blood(Game *g, float x, float y, float z, int n)
{
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            Particle *p = &g->part[i];
            if (p->life > 0.0f) continue;
            p->x = x; p->y = y; p->z = z;
            p->vx = frange(g, -2.0f, 2.0f);
            p->vy = frange(g, 0.5f, 3.5f);
            p->vz = frange(g, -2.0f, 2.0f);
            p->life = frange(g, 0.5f, 1.1f);
            p->r = (uint8_t)frange(g, 120.0f, 190.0f); p->g = 10; p->b = 10;
            break;
        }
    }
}

/* ---------- zombies ---------- */
static Zombie *spawn_zombie(Game *g, int type, float x, float z, int alerted)
{
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *q = &g->z[i];
        if (q->st != 0) continue;
        memset(q, 0, sizeof(*q));
        q->st = 1; q->type = type; q->alerted = alerted;
        q->x = x; q->z = z;
        q->yaw = frange(g, 0.0f, 2.0f * PI_F);
        q->hp = type == Z_BRUTE ? 320.0f : (type == Z_RUNNER ? 28.0f : 45.0f);
        q->side = frand(g) < 0.5f ? 1 : -1;
        q->anim = frange(g, 0.0f, 6.0f);
        static const uint8_t cloth[6][3] = {
            {70, 80, 130}, {120, 60, 50}, {90, 90, 90}, {60, 100, 70}, {130, 110, 60}, {50, 50, 60}
        };
        int c = (int)(frand(g) * 6.0f) % 6;
        q->cr = cloth[c][0]; q->cg = cloth[c][1]; q->cb = cloth[c][2];
        if (type == Z_BRUTE) { q->cr = 80; q->cg = 90; q->cb = 70; }
        return q;
    }
    return 0;
}

static int find_spawn(Game *g, float zmin, float zmax, float *ox, float *oz)
{
    for (int tries = 0; tries < 60; tries++) {
        float x = frange(g, -10.5f, 10.5f);
        float z = frange(g, zmin, zmax);
        if (game_blocked(g, x, z, 0.7f)) continue;
        float dx = x - g->p.x, dz = z - g->p.z;
        if (dx * dx + dz * dz < 14.0f * 14.0f) continue;
        *ox = x; *oz = z;
        return 1;
    }
    return 0;
}

static void spawn_horde(Game *g, int walkers, int runners, int brutes, float zc)
{
    int total = walkers + runners + brutes;
    for (int i = 0; i < total; i++) {
        int type = i < brutes ? Z_BRUTE : (i < brutes + runners ? Z_RUNNER : Z_WALKER);
        float x, z;
        int behind = (i & 1);
        float zmin = behind ? zc + 14.0f : zc - 42.0f;
        float zmax = behind ? zc + 30.0f : zc - 18.0f;
        if (zmax > 4.0f) zmax = 4.0f;
        if (find_spawn(g, zmin, zmax, &x, &z) || find_spawn(g, zc - 45.0f, zc - 16.0f, &x, &z))
            spawn_zombie(g, type, x, z, 1);
    }
    g->ev.roar++;
    g->ev.horde++;
}

static void hurt_player(Game *g, float dmg)
{
    if (g->state != ST_PLAYING) return;
    if (!g->god) g->p.hp -= dmg;
    g->p.hurt = 1.0f;
    g->ev.hurt++;
    if (g->p.hp <= 0.0f) {
        g->p.hp = 0.0f;
        g->state = ST_DEAD;
    }
}

static void kill_zombie(Game *g, Zombie *q, int head)
{
    q->st = 2;
    q->dying = 1.8f;
    g->kills++;
    if (head) g->headshots++;
    g->ev.zdie++;
    spawn_blood(g, q->x, 1.2f, q->z, 8);
}

/* ---------- level ---------- */
static void add_car(Game *g, float x, float z, int r, int gr, int b)
{
    add_wall(g, x, z, x + 2.1f, z + 4.4f, 1.5f, r, gr, b, 0);
}

static void build_level(Game *g)
{
    g->nwalls = 0;
    /* boundary walls in alternating brick tones (6 m segments) */
    for (int i = 0; i < 23; i++) {
        float z0 = -128.0f + i * 6.0f, z1 = z0 + 6.0f;
        int t = (i * 37) % 3;
        int br = 105 + t * 12, bg = 58 + t * 6, bb = 48 + t * 4;
        add_wall(g, -14.0f, z0, -12.0f, z1, 7.0f + (i % 3), br, bg, bb, 0);
        add_wall(g, 12.0f, z0, 14.0f, z1, 7.0f + ((i + 1) % 3), br - 8, bg, bb + 6, 0);
    }
    add_wall(g, -14.0f, 6.0f, 14.0f, 8.0f, 7.0f, 90, 55, 45, 0);        /* start wall */
    add_wall(g, -14.0f, -128.0f, 14.0f, -126.0f, 8.0f, 80, 80, 90, 0);  /* end wall */

    /* safe room at the far end */
    add_wall(g, -12.0f, -126.0f, -7.0f, -108.0f, 8.0f, 70, 70, 80, 0);
    add_wall(g, 7.0f, -126.0f, 12.0f, -108.0f, 8.0f, 70, 70, 80, 0);
    add_wall(g, -7.0f, -110.0f, -2.5f, -108.0f, 4.0f, 85, 115, 95, 0);
    add_wall(g, 2.5f, -110.0f, 7.0f, -108.0f, 4.0f, 85, 115, 95, 0);
    add_wall(g, -1.5f, -126.0f, 1.5f, -125.4f, 3.0f, 60, 255, 120, 1);  /* glowing exit sign */

    /* zone A: start area */
    add_car(g, -9.0f, -16.0f, 150, 40, 40);
    add_car(g, 5.0f, -22.0f, 40, 70, 150);
    add_wall(g, -3.0f, -26.0f, -1.8f, -24.8f, 1.2f, 135, 105, 60, 0);
    add_wall(g, -12.0f, -35.0f, -3.0f, -34.0f, 1.3f, 120, 120, 115, 0);
    add_wall(g, 3.0f, -46.0f, 12.0f, -45.0f, 1.3f, 120, 120, 115, 0);
    /* zone B */
    add_wall(g, -11.0f, -60.0f, -8.2f, -50.0f, 3.0f, 200, 160, 40, 0);  /* bus */
    add_car(g, 1.0f, -58.0f, 70, 140, 70);
    add_car(g, 7.0f, -66.0f, 160, 160, 160);
    add_car(g, -5.0f, -72.0f, 130, 60, 130);
    add_wall(g, 9.0f, -74.0f, 10.2f, -72.8f, 1.2f, 135, 105, 60, 0);
    add_wall(g, -12.0f, -80.0f, 6.0f, -79.0f, 1.3f, 120, 120, 115, 0);
    /* zone C */
    add_wall(g, -6.0f, -90.0f, 12.0f, -89.0f, 1.3f, 120, 120, 115, 0);
    add_car(g, 0.0f, -98.0f, 150, 100, 30);
    add_car(g, -9.0f, -102.0f, 50, 50, 60);
    add_wall(g, 6.0f, -104.0f, 7.2f, -102.8f, 1.2f, 135, 105, 60, 0);
}

static void add_pickup(Game *g, float x, float z, int kind)
{
    if (g->npick >= MAX_PICKUPS) return;
    Pickup *p = &g->pick[g->npick++];
    p->x = x; p->z = z; p->kind = kind; p->active = 1;
}

void game_reset(Game *g, uint32_t seed)
{
    memset(g, 0, sizeof(*g));
    g->rng = seed ? seed : 0x1234567u;
    build_level(g);
    fg_build(g);

    g->p.x = 0.0f; g->p.z = 0.0f; g->p.yaw = 0.0f; g->p.pitch = 0.0f;
    g->p.hp = 100.0f;
    g->p.owned[0] = 1;
    g->p.mag[0] = g_weapons[0].mag_max;

    add_pickup(g, -5.0f, -8.0f, PK_SMG);
    add_pickup(g, 8.0f, -12.0f, PK_MEDKIT);
    add_pickup(g, -6.0f, -30.0f, PK_AMMO);
    add_pickup(g, 6.0f, -40.0f, PK_SHOTGUN);
    add_pickup(g, -8.0f, -48.0f, PK_MEDKIT);
    add_pickup(g, 3.0f, -62.0f, PK_AMMO);
    add_pickup(g, -1.0f, -76.0f, PK_MEDKIT);
    add_pickup(g, 8.0f, -85.0f, PK_AMMO);
    add_pickup(g, -9.0f, -95.0f, PK_MEDKIT);
    add_pickup(g, 5.0f, -100.0f, PK_AMMO);

    g->ntrig = 4;
    g->trig[0].z = -14.0f; g->trig[1].z = -42.0f; g->trig[2].z = -70.0f; g->trig[3].z = -96.0f;

    /* idle zombies scattered along the street */
    for (int i = 0; i < 26; i++) {
        float x, z;
        int ok = 0;
        for (int t = 0; t < 40 && !ok; t++) {
            x = frange(g, -11.0f, 11.0f);
            z = frange(g, -105.0f, -18.0f);
            ok = !game_blocked(g, x, z, 0.8f);
        }
        if (ok) spawn_zombie(g, frand(g) < 0.8f ? Z_WALKER : Z_RUNNER, x, z, 0);
    }
    set_msg(g, "REACH THE SAFE ROOM");
}

void game_clear_events(Game *g) { memset(&g->ev, 0, sizeof(g->ev)); }

/* ---------- shooting ---------- */
static void fire_weapon(Game *g, const Input *in)
{
    const WeaponDef *w = &g_weapons[g->p.weapon];
    float eye[3] = { g->p.x, EYE_HEIGHT, g->p.z };
    float base[3];
    game_aim_dir(g, in, base);

    for (int k = 0; k < w->pellets; k++) {
        float d[3];
        float len = 0.0f;
        for (int i = 0; i < 3; i++) { d[i] = base[i] + frange(g, -w->spread, w->spread); len += d[i] * d[i]; }
        len = sqrtf(len);
        for (int i = 0; i < 3; i++) d[i] /= len;

        float tw = 1e9f;
        for (int i = 0; i < g->nwalls; i++) {
            const Wall *wl = &g->walls[i];
            float mn[3] = { wl->x0, 0.0f, wl->z0 }, mx[3] = { wl->x1, wl->h, wl->z1 };
            float t = ray_box(eye, d, mn, mx);
            if (t >= 0.0f && t < tw) tw = t;
        }
        if (d[1] < -1e-4f) {
            float tg = -eye[1] / d[1];
            if (tg < tw) tw = tg;
        }

        int bi = -1;
        float best = tw;
        for (int i = 0; i < MAX_ZOMBIES; i++) {
            const Zombie *q = &g->z[i];
            if (q->st != 1) continue;
            float s = q->type == Z_BRUTE ? 1.45f : 1.0f;
            float rr = 0.38f * s;
            float mn[3] = { q->x - rr, 0.0f, q->z - rr }, mx[3] = { q->x + rr, 1.85f * s, q->z + rr };
            float t = ray_box(eye, d, mn, mx);
            if (t >= 0.0f && t < best) { best = t; bi = i; }
        }
        if (bi >= 0) {
            Zombie *q = &g->z[bi];
            float s = q->type == Z_BRUTE ? 1.45f : 1.0f;
            float hy = eye[1] + d[1] * best;
            int head = hy > 1.45f * s;
            float dmg = w->dmg * (head ? 2.5f : 1.0f);
            if (w->pellets > 1 && best > 10.0f) dmg *= clampf(1.0f - (best - 10.0f) / 18.0f, 0.25f, 1.0f);
            q->hp -= dmg;
            q->flash = 0.12f;
            q->alerted = 1;
            g->p.hitmarker = 0.15f;
            if (head) g->ev.headshot++; else g->ev.hit++;
            spawn_blood(g, eye[0] + d[0] * best, hy, eye[2] + d[2] * best, head ? 5 : 3);
            if (q->hp <= 0.0f) kill_zombie(g, q, head);
        }
    }

    /* the noise wakes up idle zombies */
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *q = &g->z[i];
        if (q->st != 1 || q->alerted) continue;
        float dx = q->x - g->p.x, dz = q->z - g->p.z;
        if (dx * dx + dz * dz < w->noise * w->noise) q->alerted = 1;
    }
    if (g->p.weapon == 2) g->ev.shotgun++; else g->ev.shot++;
    g->p.mag[g->p.weapon]--;
    g->p.fire_cd = w->cd;
    g->p.muzzle = 0.06f;
    g->p.recoil = 1.0f;
}

static void start_reload(Game *g)
{
    int wi = g->p.weapon;
    const WeaponDef *w = &g_weapons[wi];
    if (g->p.reload_t > 0.0f || g->p.mag[wi] >= w->mag_max) return;
    if (w->reserve_max >= 0 && g->p.reserve[wi] <= 0) return;
    g->p.reload_t = w->reload;
    g->ev.reload++;
}

static void finish_reload(Game *g)
{
    int wi = g->p.weapon;
    const WeaponDef *w = &g_weapons[wi];
    int need = w->mag_max - g->p.mag[wi];
    if (w->reserve_max < 0) { g->p.mag[wi] = w->mag_max; return; }
    int take = need < g->p.reserve[wi] ? need : g->p.reserve[wi];
    g->p.mag[wi] += take;
    g->p.reserve[wi] -= take;
}

static void change_weapon(Game *g, int dir)
{
    for (int k = 1; k <= 3; k++) {
        int n = (g->p.weapon + dir * k + 6) % 3;
        if (g->p.owned[n]) {
            g->p.weapon = n;
            g->p.reload_t = 0.0f;
            g->p.fire_cd = 0.25f;
            return;
        }
    }
}

/* ---------- main update ---------- */
void game_update(Game *g, const Input *in, float dt)
{
    Player *p = &g->p;

    /* particles always move */
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *q = &g->part[i];
        if (q->life <= 0.0f) continue;
        q->life -= dt;
        q->vy -= 9.0f * dt;
        q->x += q->vx * dt; q->y += q->vy * dt; q->z += q->vz * dt;
        if (q->y < 0.03f) { q->y = 0.03f; q->vx *= 0.3f; q->vz *= 0.3f; q->vy = 0.0f; }
    }
    if (g->msg_t > 0.0f) g->msg_t -= dt;
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *q = &g->z[i];
        if (q->st == 2) { q->dying -= dt; if (q->dying <= 0.0f) q->st = 0; }
        if (q->flash > 0.0f) q->flash -= dt;
    }
    if (p->hurt > 0.0f) p->hurt -= dt * 1.6f;
    if (p->hitmarker > 0.0f) p->hitmarker -= dt;
    if (p->muzzle > 0.0f) p->muzzle -= dt;
    if (p->recoil > 0.0f) p->recoil -= dt * 8.0f;

    if (g->state != ST_PLAYING) return;
    g->time += dt;

    /* look */
    p->yaw += in->yaw_rate * dt;
    p->pitch = clampf(p->pitch + in->pitch_rate * dt, -1.0f, 1.0f);

    /* move */
    {
        float sy = sinf(p->yaw), cy = cosf(p->yaw);
        float vx = sy * in->move_f + cy * in->move_s;
        float vz = -cy * in->move_f + sy * in->move_s;
        float len = sqrtf(vx * vx + vz * vz);
        if (len > 1.0f) { vx /= len; vz /= len; }
        float speed = in->sprint ? 6.2f : 4.2f;
        move_circle(g, &p->x, &p->z, vx * speed * dt, vz * speed * dt, 0.4f);
    }

    /* weapons */
    if (in->weapon_next) change_weapon(g, 1);
    if (in->weapon_prev) change_weapon(g, -1);
    if (p->fire_cd > 0.0f) p->fire_cd -= dt;
    if (p->reload_t > 0.0f) {
        p->reload_t -= dt;
        if (p->reload_t <= 0.0f) { p->reload_t = 0.0f; finish_reload(g); }
    }
    if (in->reload) start_reload(g);
    {
        const WeaponDef *w = &g_weapons[p->weapon];
        int want = w->auto_fire ? in->fire : in->fire_pressed;
        if (want && p->reload_t <= 0.0f && p->fire_cd <= 0.0f) {
            if (p->mag[p->weapon] > 0) {
                fire_weapon(g, in);
                if (p->mag[p->weapon] == 0) start_reload(g);
            } else {
                g->ev.click++;
                p->fire_cd = 0.3f;
                start_reload(g);
            }
        }
    }

    /* horde triggers */
    for (int i = 0; i < g->ntrig; i++) {
        Trigger *t = &g->trig[i];
        if (t->fired || p->z > t->z) continue;
        t->fired = 1;
        if (i == 0) { spawn_horde(g, 6, 1, 0, t->z); set_msg(g, "HORDE INCOMING!"); }
        if (i == 1) { spawn_horde(g, 7, 4, 0, t->z); set_msg(g, "HORDE INCOMING!"); }
        if (i == 2) { spawn_horde(g, 5, 2, 1, t->z); set_msg(g, "A BRUTE IS COMING!"); }
        if (i == 3) { spawn_horde(g, 10, 6, 0, t->z); set_msg(g, "FINAL HORDE!"); }
    }

    /* pickups */
    for (int i = 0; i < g->npick; i++) {
        Pickup *k = &g->pick[i];
        if (!k->active) continue;
        float dx = k->x - p->x, dz = k->z - p->z;
        if (dx * dx + dz * dz > 1.3f * 1.3f) continue;
        if (k->kind == PK_MEDKIT) {
            if (p->hp >= 100.0f) continue;
            p->hp = p->hp + 50.0f > 100.0f ? 100.0f : p->hp + 50.0f;
            set_msg(g, "MEDKIT +50");
        } else if (k->kind == PK_AMMO) {
            if (!p->owned[1] && !p->owned[2]) continue;
            if (p->owned[1]) { p->reserve[1] += 90; if (p->reserve[1] > g_weapons[1].reserve_max) p->reserve[1] = g_weapons[1].reserve_max; }
            if (p->owned[2]) { p->reserve[2] += 16; if (p->reserve[2] > g_weapons[2].reserve_max) p->reserve[2] = g_weapons[2].reserve_max; }
            set_msg(g, "AMMO");
        } else if (k->kind == PK_SMG) {
            int first = !p->owned[1];
            p->owned[1] = 1;
            if (first) p->mag[1] = g_weapons[1].mag_max;
            p->reserve[1] += 135; if (p->reserve[1] > g_weapons[1].reserve_max) p->reserve[1] = g_weapons[1].reserve_max;
            if (first) { p->weapon = 1; p->reload_t = 0.0f; }
            set_msg(g, "GOT THE SMG");
        } else {
            int first = !p->owned[2];
            p->owned[2] = 1;
            if (first) p->mag[2] = g_weapons[2].mag_max;
            p->reserve[2] += 24; if (p->reserve[2] > g_weapons[2].reserve_max) p->reserve[2] = g_weapons[2].reserve_max;
            if (first) { p->weapon = 2; p->reload_t = 0.0f; }
            set_msg(g, "GOT THE SHOTGUN");
        }
        k->active = 0;
        g->ev.pickup++;
    }

    /* zombies */
    fg_timer -= dt;
    if (fg_timer <= 0.0f) { fg_timer = 0.25f; fg_update(g); }
    int any_close = 0;
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *q = &g->z[i];
        if (q->st != 1) continue;
        float s = q->type == Z_BRUTE ? 1.45f : 1.0f;
        float dx = p->x - q->x, dz = p->z - q->z;
        float d = sqrtf(dx * dx + dz * dz);
        if (!q->alerted) {
            if (d < 11.0f) q->alerted = 1;
            else { q->anim += dt * 0.6f; continue; }
        }
        if (d < 22.0f) any_close = 1;
        float want = atan2f(dx, -dz);
        float diff = want - q->yaw;
        while (diff > PI_F) diff -= 2.0f * PI_F;
        while (diff < -PI_F) diff += 2.0f * PI_F;
        q->yaw += diff * clampf(dt * 8.0f, 0.0f, 1.0f);

        float reach = 0.95f * s + 0.35f;
        if (d > reach) {
            float speed = q->type == Z_RUNNER ? 4.0f : (q->type == Z_BRUTE ? 2.8f : 1.8f);
            float nx = dx / d, nz = dz / d;
            float mx = nx, mz = nz;
            if (d > 2.0f) {                    /* far away: follow the flow field around obstacles */
                float fx, fz;
                if (fg_direction(q->x, q->z, &fx, &fz)) { mx = fx; mz = fz; }
            }
            if (q->stuck > 0.5f) {             /* still blocked: sidestep along the obstacle */
                mx = -nz * q->side; mz = nx * q->side;
                if (q->stuck > 1.5f) { q->side = -q->side; q->stuck = 0.0f; }
            }
            float ox = q->x, oz = q->z;
            move_circle(g, &q->x, &q->z, mx * speed * dt, mz * speed * dt, 0.35f * s);
            float moved = sqrtf((q->x - ox) * (q->x - ox) + (q->z - oz) * (q->z - oz));
            if (moved < speed * dt * 0.3f) q->stuck += dt; else q->stuck -= dt * 2.0f;
            if (q->stuck < 0.0f) q->stuck = 0.0f;
            q->anim += speed * dt * 2.4f;
        } else {
            q->cd -= dt;
            if (q->cd <= 0.0f) {
                if (q->type == Z_BRUTE) { hurt_player(g, 28.0f); q->cd = 1.5f; }
                else if (q->type == Z_RUNNER) { hurt_player(g, 5.0f); q->cd = 0.8f; }
                else { hurt_player(g, 7.0f); q->cd = 1.1f; }
            }
        }
    }
    /* keep zombies from stacking on each other */
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *a = &g->z[i];
        if (a->st != 1 || !a->alerted) continue;
        for (int j = i + 1; j < MAX_ZOMBIES; j++) {
            Zombie *b = &g->z[j];
            if (b->st != 1) continue;
            float dx = b->x - a->x, dz = b->z - a->z;
            float d2 = dx * dx + dz * dz;
            if (d2 > 0.5f * 0.5f || d2 < 1e-6f) continue;
            float d = sqrtf(d2);
            float push = (0.5f - d) * 0.5f;
            move_circle(g, &b->x, &b->z, dx / d * push, dz / d * push, 0.3f);
            move_circle(g, &a->x, &a->z, -dx / d * push, -dz / d * push, 0.3f);
        }
    }
    if (any_close && frand(g) < dt * 0.5f) g->ev.groan++;

    /* win check */
    if (p->z < -114.0f && fabsf(p->x) < 6.5f) g->state = ST_WON;
}
