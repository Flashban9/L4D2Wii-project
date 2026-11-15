/* Headless test: level reachability + a bot that plays the game */
#include "../source/game.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define CELL 0.25f
#define GX0 -14.0f
#define GZ0 -128.0f
#define NX 112
#define NZ 544

static unsigned char blocked_grid[NZ][NX];
static int dist_grid[NZ][NX];

static void cell_of(float x, float z, int *cx, int *cz) { *cx = (int)((x - GX0) / CELL); *cz = (int)((z - GZ0) / CELL); }

int main(int argc, char **argv)
{
    static Game g;
    int seeds = argc > 1 ? atoi(argv[1]) : 5;
    game_reset(&g, 1234);
    printf("walls=%d pickups=%d idle zombies=%d\n", g.nwalls, g.npick, game_alive_count(&g));

    /* 1) reachability with inflated walls (player radius 0.4 + margin) */
    for (int j = 0; j < NZ; j++) for (int i = 0; i < NX; i++)
        blocked_grid[j][i] = (unsigned char)game_blocked(&g, GX0 + (i + 0.5f) * CELL, GZ0 + (j + 0.5f) * CELL, 0.45f);
    int sx, sz; cell_of(0.0f, -118.0f, &sx, &sz);   /* BFS from the safe room outward */
    memset(dist_grid, -1, sizeof(dist_grid));
    static int qx[NX * NZ], qz[NX * NZ]; int qh = 0, qt = 0;
    dist_grid[sz][sx] = 0; qx[qt] = sx; qz[qt++] = sz;
    while (qh < qt) {
        int x = qx[qh], z = qz[qh++];
        static const int dx[4] = {1,-1,0,0}, dz[4] = {0,0,1,-1};
        for (int k = 0; k < 4; k++) {
            int nx = x + dx[k], nz = z + dz[k];
            if (nx < 0 || nz < 0 || nx >= NX || nz >= NZ) continue;
            if (blocked_grid[nz][nx] || dist_grid[nz][nx] >= 0) continue;
            dist_grid[nz][nx] = dist_grid[z][x] + 1;
            qx[qt] = nx; qz[qt++] = nz;
        }
    }
    int gx, gz; cell_of(0.0f, 0.0f, &gx, &gz);
    printf("safe room reachable: %s (path length %.1f m)\n", dist_grid[gz][gx] >= 0 ? "YES" : "NO", dist_grid[gz][gx] * CELL);
    int bad = 0;
    for (int i = 0; i < g.npick; i++) {
        int cx, cz; cell_of(g.pick[i].x, g.pick[i].z, &cx, &cz);
        if (dist_grid[cz][cx] < 0) { printf("pickup %d unreachable (%.1f, %.1f)\n", i, g.pick[i].x, g.pick[i].z); bad++; }
    }
    for (int i = 0; i < MAX_ZOMBIES; i++) if (g.z[i].st == 1) {
        int cx, cz; cell_of(g.z[i].x, g.z[i].z, &cx, &cz);
        if (dist_grid[cz][cx] < 0) { printf("zombie %d in unreachable spot\n", i); bad++; }
    }
    printf("unreachable pickups/zombies: %d\n", bad);

    /* 2) bot plays with god mode */
    int won = 0, maxz = 0, nan_seen = 0; float total_time = 0; int kills_sum = 0, stuck_max = 0;
    for (int s = 0; s < seeds; s++) {
        game_reset(&g, 1000 + s * 77);
        g.god = argc > 2 ? atoi(argv[2]) : 1;
        Input in; memset(&in, 0, sizeof(in));
        in.tan_y = tanf(30.0f * 3.14159265f / 180.0f); in.tan_x = in.tan_y * 16.0f / 9.0f;
        float t = 0;
        for (int step = 0; step < 60 * 400 && g.state == ST_PLAYING; step++, t += 1.0f / 60.0f) {
            memset(&in.move_f, 0, 0);
            /* nearest alerted zombie */
            int bi = -1; float bd = 1e9f;
            for (int i = 0; i < MAX_ZOMBIES; i++) if (g.z[i].st == 1) {
                float dx = g.z[i].x - g.p.x, dz = g.z[i].z - g.p.z, d = sqrtf(dx*dx+dz*dz);
                if (d < bd && d < 26.0f) { bd = d; bi = i; }
            }
            if (bi >= 0) {
                float dx = g.z[bi].x - g.p.x, dz = g.z[bi].z - g.p.z;
                g.p.yaw = atan2f(dx, -dz);
                g.p.pitch = atan2f(1.2f - 1.6f, bd);
                in.fire = 1; in.fire_pressed = (step % 12) == 0; in.reload = g.p.mag[g.p.weapon] < 3;
            } else { in.fire = 0; in.fire_pressed = 0; in.reload = g.p.mag[g.p.weapon] < 10; }
            in.weapon_next = (step % 300 == 0) && g.p.owned[2];
            /* follow the distance field downhill, looking a few cells ahead */
            int cx, cz; cell_of(g.p.x, g.p.z, &cx, &cz);
            int bx = cx, bz = cz, bestd = dist_grid[cz][cx] < 0 ? (1 << 30) : dist_grid[cz][cx];
            for (int oz = -6; oz <= 6; oz++) for (int ox = -6; ox <= 6; ox++) {
                int nx = cx + ox, nz = cz + oz;
                if (nx < 0 || nz < 0 || nx >= NX || nz >= NZ || dist_grid[nz][nx] < 0) continue;
                /* only cells that are visible along a straight free line */
                int free_line = 1;
                for (int k = 1; k <= 6 && free_line; k++) {
                    int ix = cx + ox * k / 6, iz = cz + oz * k / 6;
                    if (blocked_grid[iz][ix]) free_line = 0;
                }
                if (free_line && dist_grid[nz][nx] < bestd) { bestd = dist_grid[nz][nx]; bx = nx; bz = nz; }
            }
            float tx = GX0 + (bx + 0.5f) * CELL, tz = GZ0 + (bz + 0.5f) * CELL;
            float wx = tx - g.p.x, wz = tz - g.p.z, wl = sqrtf(wx*wx + wz*wz); if (wl > 0.01f) { wx /= wl; wz /= wl; }
            float sy = sinf(g.p.yaw), cy = cosf(g.p.yaw);
            in.move_f = sy * wx - cy * wz; in.move_s = cy * wx + sy * wz;
            if (bi >= 0 && bd < 12.0f) { in.move_f = 0; in.move_s = 0; }   /* stand and fight like a player would */
            else if (bi >= 0 && bd < 4.0f) { in.move_f *= 0.3f; in.move_s *= 0.3f; }
            game_update(&g, &in, 1.0f / 60.0f);
            game_clear_events(&g);
            int alive = game_alive_count(&g); if (alive > maxz) maxz = alive;
            if (!isfinite(g.p.x) || !isfinite(g.p.z)) nan_seen = 1;
            int st = 0; for (int i = 0; i < MAX_ZOMBIES; i++) if (g.z[i].st == 1 && g.z[i].stuck > 3.0f) st++;
            if (st > stuck_max) stuck_max = st;
        }
        printf("seed %d: state=%d time=%.0fs pos=(%.1f,%.1f) kills=%d hp=%.0f\n", s, g.state, t, g.p.x, g.p.z, g.kills, g.p.hp);
        if (g.state == ST_WON) { won++; total_time += t; }
        kills_sum += g.kills;
    }
    printf("won %d/%d, max zombies alive=%d, stuck-zombie peak=%d, NaN=%d, avg kills=%d\n", won, seeds, maxz, stuck_max, nan_seen, kills_sum / seeds);
    return 0;
}
