/*
 * UNDEAD RUSH - a low poly zombie horde shooter for the Nintendo Wii
 * Created by Flashban9
 *
 * Wiimote + Nunchuk: stick = move, pointer = aim (screen edge turns), B = fire, A = reload,
 * C = sprint, Z = switch weapon, HOME = back to the Wii menu.
 */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <asndlib.h>
#include <ogc/conf.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

#include "game.h"

#define DEFAULT_FIFO_SIZE (256 * 1024)
#define PI_F 3.14159265f

static GXRModeObj *rmode;
static void *xfb[2];
static int fbi = 0;
static void *gp_fifo;
static float aspect = 4.0f / 3.0f;
static volatile int quit_flag = 0;

static Game G;

/* ================= small helpers ================= */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static u8 col8(float v) { return (u8)clampf(v, 0.0f, 255.0f); }

static void on_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; quit_flag = 1; }
static void on_power(void) { quit_flag = 2; }

/* ================= video / GX ================= */
static void video_init(void)
{
    VIDEO_Init();
    CONF_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    aspect = (CONF_GetAspectRatio() == CONF_ASPECT_16_9) ? 16.0f / 9.0f : 4.0f / 3.0f;

    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb[fbi]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    gp_fifo = memalign(32, DEFAULT_FIFO_SIZE);
    memset(gp_fifo, 0, DEFAULT_FIFO_SIZE);
    GX_Init(gp_fifo, DEFAULT_FIFO_SIZE);

    GXColor bg = { 14, 16, 26, 255 };
    GX_SetCopyClear(bg, GX_MAX_Z24);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    u32 xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering, ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    if (rmode->aa) GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
    else GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_CopyDisp(xfb[fbi], GX_TRUE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    /* one simple vertex format: position + colour, no lighting, no textures */
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetNumChans(1);
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

/* ================= low poly drawing ================= */
static float cam_x, cam_y, cam_z;

/*
 * Axis-oriented box. Offsets and half sizes are in (right, up, forward) of the box;
 * yaw turns the box around the Y axis (same convention as the game: yaw 0 looks toward -Z).
 */
static void draw_box(float cx, float cy, float cz, float hx, float hy, float hf, float yaw,
                     int r, int g, int b, float shade)
{
    float sn = sinf(yaw), cs = cosf(yaw);
    float R[3] = { cs, 0.0f, sn };
    float F[3] = { sn, 0.0f, -cs };
    float c[8][3];
    for (int i = 0; i < 8; i++) {
        float sx = (i & 1) ? 1.0f : -1.0f;
        float sy = (i & 2) ? 1.0f : -1.0f;
        float sf = (i & 4) ? 1.0f : -1.0f;
        c[i][0] = cx + sx * hx * R[0] + sf * hf * F[0];
        c[i][1] = cy + sy * hy;
        c[i][2] = cz + sx * hx * R[2] + sf * hf * F[2];
    }
    static const int face[5][4] = {
        { 2, 3, 7, 6 },   /* top */
        { 1, 3, 7, 5 },   /* right */
        { 0, 2, 6, 4 },   /* left */
        { 4, 5, 7, 6 },   /* front */
        { 0, 1, 3, 2 },   /* back */
    };
    static const float mult[5] = { 1.00f, 0.78f, 0.78f, 0.66f, 0.66f };

    GX_Begin(GX_QUADS, GX_VTXFMT0, 20);
    for (int f = 0; f < 5; f++) {
        u8 rr = col8(r * shade * mult[f]), gg = col8(g * shade * mult[f]), bb = col8(b * shade * mult[f]);
        for (int k = 0; k < 4; k++) {
            const float *v = c[face[f][k]];
            GX_Position3f32(v[0], v[1], v[2]);
            GX_Color4u8(rr, gg, bb, 255);
        }
    }
    GX_End();
}

static float fade_for(float x, float z)
{
    float dx = x - cam_x, dz = z - cam_z;
    return clampf(1.0f - sqrtf(dx * dx + dz * dz) / 58.0f, 0.10f, 1.0f);
}

static void draw_wall(const Wall *w)
{
    float cx = clampf(cam_x, w->x0, w->x1), cz = clampf(cam_z, w->z0, w->z1);
    float shade = w->glow ? 1.0f : fade_for(cx, cz);
    draw_box((w->x0 + w->x1) * 0.5f, w->h * 0.5f, (w->z0 + w->z1) * 0.5f,
             (w->x1 - w->x0) * 0.5f, w->h * 0.5f, (w->z1 - w->z0) * 0.5f, 0.0f,
             w->r, w->g, w->b, shade);
}

static void draw_zombie(const Zombie *q)
{
    float s = q->type == Z_BRUTE ? 1.45f : 1.0f;
    float shade = fade_for(q->x, q->z);
    float squash = 1.0f;
    float sink = 0.0f;
    if (q->st == 2) {                       /* collapse when dead */
        float f = 1.0f - q->dying / 1.8f;
        squash = clampf(1.0f - f * 3.0f, 0.12f, 1.0f);
        sink = 0.0f;
    }
    int sr = 110, sg = 140, sb = 100;       /* skin */
    int br = q->cr, bg = q->cg, bb = q->cb;
    if (q->type == Z_RUNNER) { sr = 150; sg = 130; sb = 90; }
    if (q->flash > 0.0f) { sr = sg = sb = 255; br = bg = bb = 255; }

    float swing = q->alerted ? sinf(q->anim) * 0.35f : sinf(q->anim) * 0.05f;
    float lx = q->x, lz = q->z, yaw = q->yaw;

    /* every part: offsets are (right, up, forward) in zombie space */
    #define PART(ox, oy, oz, hx, hy, hf, R, Gc, B) \
        do { \
            float _sn = sinf(yaw), _cs = cosf(yaw); \
            float _wx = lx + (ox) * s * _cs + (oz) * s * _sn; \
            float _wz = lz + (ox) * s * _sn - (oz) * s * _cs; \
            draw_box(_wx, (oy) * s * squash + sink, _wz, (hx) * s, (hy) * s * squash, (hf) * s, yaw, R, Gc, B, shade); \
        } while (0)

    PART(-0.16f, 0.42f, swing * 0.8f, 0.10f, 0.42f, 0.10f, 50, 50, 70);     /* legs */
    PART(0.16f, 0.42f, -swing * 0.8f, 0.10f, 0.42f, 0.10f, 50, 50, 70);
    PART(0.0f, 1.12f, 0.0f, 0.26f, 0.32f, 0.14f, br, bg, bb);                /* torso */
    PART(0.0f, 1.62f, 0.04f, 0.15f, 0.16f, 0.15f, sr, sg, sb);               /* head */
    if (q->alerted && q->st == 1) {
        PART(-0.36f, 1.28f, 0.38f, 0.07f, 0.07f, 0.36f, sr, sg, sb);         /* arms stretched out */
        PART(0.36f, 1.28f, 0.38f, 0.07f, 0.07f, 0.36f, sr, sg, sb);
    } else {
        PART(-0.34f, 1.02f, 0.0f, 0.07f, 0.34f, 0.07f, sr, sg, sb);          /* arms hanging down */
        PART(0.34f, 1.02f, 0.0f, 0.07f, 0.34f, 0.07f, sr, sg, sb);
    }
    #undef PART
}

static void draw_pickup(const Pickup *k, float t)
{
    float bob = 0.45f + 0.08f * sinf(t * 3.0f + k->x);
    float shade = clampf(fade_for(k->x, k->z) + 0.25f, 0.2f, 1.0f);
    float yaw = t * 1.8f;
    switch (k->kind) {
    case PK_MEDKIT:
        draw_box(k->x, bob, k->z, 0.28f, 0.14f, 0.2f, yaw, 235, 235, 235, shade);
        draw_box(k->x, bob + 0.13f, k->z, 0.18f, 0.03f, 0.06f, yaw, 220, 30, 30, shade);
        draw_box(k->x, bob + 0.13f, k->z, 0.06f, 0.03f, 0.18f, yaw, 220, 30, 30, shade);
        break;
    case PK_AMMO:
        draw_box(k->x, bob, k->z, 0.25f, 0.15f, 0.18f, yaw, 110, 120, 60, shade);
        draw_box(k->x, bob + 0.16f, k->z, 0.2f, 0.02f, 0.13f, yaw, 230, 200, 60, shade);
        break;
    default:
        draw_box(k->x, bob, k->z, 0.08f, 0.08f, 0.45f, yaw, 60, 60, 70, shade);
        draw_box(k->x, bob + 0.11f, k->z, 0.05f, 0.03f, 0.25f, yaw,
                 k->kind == PK_SMG ? 230 : 160, k->kind == PK_SMG ? 200 : 100, 60, shade);
        break;
    }
}

static void draw_world(float eye_y)
{
    /* floor strips fade into the dark with distance */
    for (float z = 8.0f; z > -128.0f; z -= 8.0f) {
        float f = fade_for(0.0f, z - 4.0f);
        draw_box(0.0f, -0.06f, z - 4.0f, 14.0f, 0.06f, 4.0f, 0.0f, 62, 62, 70, f);
    }
    draw_box(0.0f, -0.05f, -118.0f, 7.0f, 0.06f, 8.0f, 0.0f, 55, 110, 80, 0.9f);   /* safe room floor */
    for (float z = -3.0f; z > -108.0f; z -= 6.0f)
        draw_box(0.0f, 0.02f, z, 0.08f, 0.01f, 1.2f, 0.0f, 215, 185, 40, fade_for(0.0f, z));

    for (int i = 0; i < G.nwalls; i++) draw_wall(&G.walls[i]);
    for (int i = 0; i < G.npick; i++) if (G.pick[i].active) draw_pickup(&G.pick[i], G.time);
    for (int i = 0; i < MAX_ZOMBIES; i++) if (G.z[i].st != 0) draw_zombie(&G.z[i]);
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &G.part[i];
        if (p->life > 0.0f) draw_box(p->x, p->y, p->z, 0.05f, 0.05f, 0.05f, 0.0f, p->r, p->g, p->b, 1.0f);
    }
    (void)eye_y;
}

/* the weapon in your hands, drawn in camera space */
static void draw_gun(void)
{
    const Player *p = &G.p;
    float rec = clampf(p->recoil, 0.0f, 1.0f);
    float bx = 0.20f + G.p.hitmarker * 0.0f;
    float by = -0.20f + rec * 0.02f;
    float bz = -0.50f + rec * 0.06f;
    if (p->reload_t > 0.0f) by -= 0.12f;     /* lower the gun while reloading */
    switch (p->weapon) {
    case 0:
        draw_box(bx, by, bz, 0.03f, 0.04f, 0.14f, 0.0f, 55, 55, 60, 1.0f);
        draw_box(bx, by - 0.06f, bz + 0.07f, 0.025f, 0.05f, 0.03f, 0.0f, 40, 40, 45, 1.0f);
        if (p->muzzle > 0.0f) draw_box(bx, by + 0.01f, bz - 0.20f, 0.03f, 0.03f, 0.05f, 0.0f, 255, 220, 100, 1.4f);
        break;
    case 1:
        draw_box(bx, by, bz - 0.05f, 0.035f, 0.045f, 0.22f, 0.0f, 45, 50, 45, 1.0f);
        draw_box(bx, by - 0.07f, bz + 0.02f, 0.025f, 0.06f, 0.025f, 0.0f, 35, 35, 35, 1.0f);
        draw_box(bx, by - 0.01f, bz + 0.22f, 0.025f, 0.035f, 0.08f, 0.0f, 90, 70, 40, 1.0f);
        if (p->muzzle > 0.0f) draw_box(bx, by + 0.01f, bz - 0.32f, 0.03f, 0.03f, 0.05f, 0.0f, 255, 220, 100, 1.4f);
        break;
    default:
        draw_box(bx, by, bz - 0.1f, 0.03f, 0.035f, 0.32f, 0.0f, 60, 60, 65, 1.0f);
        draw_box(bx, by - 0.02f, bz + 0.15f, 0.03f, 0.045f, 0.12f, 0.0f, 120, 80, 40, 1.0f);
        if (p->muzzle > 0.0f) draw_box(bx, by + 0.01f, bz - 0.46f, 0.05f, 0.05f, 0.06f, 0.0f, 255, 220, 100, 1.4f);
        break;
    }
}

/* ================= HUD ================= */
static void hud_rect(float x, float y, float w, float h, int r, int g, int b, int a)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x, y, -1.0f);         GX_Color4u8(r, g, b, a);
    GX_Position3f32(x + w, y, -1.0f);     GX_Color4u8(r, g, b, a);
    GX_Position3f32(x + w, y + h, -1.0f); GX_Color4u8(r, g, b, a);
    GX_Position3f32(x, y + h, -1.0f);     GX_Color4u8(r, g, b, a);
    GX_End();
}

/* 3x5 pixel font, rows top to bottom */
static const char font_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!:/-.+";
static const char *const font_bits[] = {
    "010101111101101", "110101110101110", "011100100100011", "110101101101110", "111100110100111",
    "111100110100100", "011100101101011", "101101111101101", "111010010010111", "001001001101010",
    "101101110101101", "100100100100111", "101111111101101", "110101101101101", "010101101101010",
    "110101110100100", "010101101110011", "110101110101101", "011100010001110", "111010010010010",
    "101101101101111", "101101101101010", "101101111111101", "101101010101101", "101101010010010",
    "111001010100111",
    "111101101101111", "010110010010111", "110001010100111", "110001010001110", "101101111001001",
    "111100110001110", "011100111101111", "111001010010010", "111101111101111", "111101111001110",
    "010010010000010", "000010000010000", "001001010100100", "000000111000000", "000000000000010",
    "000010111010000",
};

static float text_width(const char *s, float px) { return (float)strlen(s) * px * 4.0f; }

static void hud_text_raw(float x, float y, float px, const char *s, int r, int g, int b, int a)
{
    for (; *s; s++, x += px * 4.0f) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ') continue;
        const char *pos = strchr(font_chars, c);
        if (!pos) continue;
        const char *bits = font_bits[pos - font_chars];
        int n = 0;
        for (int k = 0; k < 15; k++) if (bits[k] == '1') n++;
        GX_Begin(GX_QUADS, GX_VTXFMT0, n * 4);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++) {
                if (bits[row * 3 + col] != '1') continue;
                float x0 = x + col * px, y0 = y + row * px, x1 = x0 + px, y1 = y0 + px;
                GX_Position3f32(x0, y0, -1.0f); GX_Color4u8(r, g, b, a);
                GX_Position3f32(x1, y0, -1.0f); GX_Color4u8(r, g, b, a);
                GX_Position3f32(x1, y1, -1.0f); GX_Color4u8(r, g, b, a);
                GX_Position3f32(x0, y1, -1.0f); GX_Color4u8(r, g, b, a);
            }
        GX_End();
    }
}

static void hud_text(float x, float y, float px, const char *s, int r, int g, int b)
{
    hud_text_raw(x + px * 0.6f, y + px * 0.6f, px, s, 0, 0, 0, 200);     /* shadow */
    hud_text_raw(x, y, px, s, r, g, b, 255);
}

static void hud_text_center(float y, float px, const char *s, int r, int g, int b)
{
    hud_text(320.0f - text_width(s, px) * 0.5f, y, px, s, r, g, b);
}

static float ptr_x = 0.0f, ptr_y = 0.0f;       /* smoothed pointer, -1..1 */

static void draw_hud(int title, float dead_t)
{
    char buf[64];
    const Player *p = &G.p;

    if (p->hurt > 0.0f) hud_rect(0, 0, 640, 480, 180, 0, 0, (int)clampf(p->hurt * 110.0f, 0.0f, 200.0f));
    if (p->hp < 30.0f && G.state == ST_PLAYING) hud_rect(0, 0, 640, 480, 120, 0, 0, 35);

    /* crosshair follows the pointer */
    float cx = 320.0f + ptr_x * 320.0f, cy = 240.0f - ptr_y * 240.0f;
    int hm = p->hitmarker > 0.0f;
    int cr = hm ? 255 : 240, cg = hm ? 60 : 240, cb = hm ? 60 : 240;
    float gap = hm ? 7.0f : 5.0f;
    hud_rect(cx - gap - 9, cy - 1, 9, 2, cr, cg, cb, 230);
    hud_rect(cx + gap, cy - 1, 9, 2, cr, cg, cb, 230);
    hud_rect(cx - 1, cy - gap - 9, 2, 9, cr, cg, cb, 230);
    hud_rect(cx - 1, cy + gap, 2, 9, cr, cg, cb, 230);
    hud_rect(cx - 1, cy - 1, 2, 2, cr, cg, cb, 255);

    /* health */
    float hp = clampf(p->hp, 0.0f, 100.0f);
    hud_rect(24, 430, 204, 22, 0, 0, 0, 170);
    hud_rect(26, 432, 2.0f * hp, 18, col8(255 - hp * 2.0f), col8(60 + hp * 1.8f), 40, 235);
    hud_text(26, 410, 3, "HEALTH", 255, 255, 255);
    snprintf(buf, sizeof(buf), "%d", (int)(hp + 0.5f));
    hud_text(236, 432, 3, buf, 255, 255, 255);

    /* weapon + ammo */
    const WeaponDef *w = &g_weapons[p->weapon];
    hud_text(640 - 24 - text_width(w->name, 3), 392, 3, w->name, 255, 230, 120);
    if (w->reserve_max < 0) snprintf(buf, sizeof(buf), "%d / INF", p->mag[p->weapon]);
    else snprintf(buf, sizeof(buf), "%d / %d", p->mag[p->weapon], p->reserve[p->weapon]);
    hud_text(640 - 24 - text_width(buf, 5), 418, 5, buf, 255, 255, 255);
    if (p->reload_t > 0.0f) hud_text_center(330, 4, "RELOADING", 255, 200, 80);

    snprintf(buf, sizeof(buf), "KILLS %d", G.kills);
    hud_text(24, 24, 3, buf, 255, 255, 255);

    if (G.msg_t > 0.0f && !title) hud_text_center(70, 5, G.msg, 255, 90, 60);

    if (title) {
        hud_rect(0, 0, 640, 480, 0, 0, 0, 170);
        hud_text_center(50, 9, "UNDEAD RUSH", 220, 40, 40);
        hud_text_center(110, 3, "A LOW POLY ZOMBIE SHOOTER FOR THE WII", 230, 230, 230);
        hud_text_center(160, 3, "REACH THE SAFE ROOM AT THE END OF THE STREET", 255, 220, 120);
        hud_text(150, 210, 3, "STICK       MOVE", 255, 255, 255);
        hud_text(150, 235, 3, "POINTER     AIM - SCREEN EDGE TURNS", 255, 255, 255);
        hud_text(150, 260, 3, "B           FIRE", 255, 255, 255);
        hud_text(150, 285, 3, "A           RELOAD", 255, 255, 255);
        hud_text(150, 310, 3, "C           SPRINT", 255, 255, 255);
        hud_text(150, 335, 3, "Z           SWITCH WEAPON", 255, 255, 255);
        hud_text(150, 360, 3, "HOME        EXIT", 255, 255, 255);
        hud_text_center(410, 4, "PRESS A TO START", 120, 255, 140);
    } else if (G.state == ST_DEAD) {
        hud_rect(0, 0, 640, 480, 90, 0, 0, (int)clampf(dead_t * 120.0f, 0.0f, 170.0f));
        if (dead_t > 0.8f) {
            hud_text_center(140, 10, "YOU DIED", 230, 30, 30);
            snprintf(buf, sizeof(buf), "KILLS %d   HEADSHOTS %d", G.kills, G.headshots);
            hud_text_center(250, 4, buf, 255, 255, 255);
            hud_text_center(330, 4, "PRESS A TO TRY AGAIN", 255, 220, 120);
        }
    } else if (G.state == ST_WON) {
        hud_rect(0, 0, 640, 480, 0, 60, 20, 120);
        hud_text_center(120, 8, "SAFE ROOM REACHED!", 100, 255, 140);
        snprintf(buf, sizeof(buf), "KILLS %d   HEADSHOTS %d", G.kills, G.headshots);
        hud_text_center(230, 4, buf, 255, 255, 255);
        snprintf(buf, sizeof(buf), "TIME %d:%02d   HEALTH LEFT %d", (int)G.time / 60, (int)G.time % 60, (int)(G.p.hp + 0.5f));
        hud_text_center(280, 4, buf, 255, 255, 255);
        hud_text_center(350, 4, "PRESS A TO PLAY AGAIN", 255, 220, 120);
    }
}

/* ================= render a whole frame ================= */
static void render_frame(int title, float bob, float dead_t)
{
    Mtx view, ident;
    Mtx44 proj, ortho;

    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_InvVtxCache();
    GX_InvalidateTexAll();

    float eye_y = EYE_HEIGHT + 0.035f * sinf(bob);
    if (G.state == ST_DEAD) eye_y = EYE_HEIGHT - 1.1f * clampf(dead_t * 1.5f, 0.0f, 1.0f);
    cam_x = G.p.x; cam_y = eye_y; cam_z = G.p.z;

    float sy = sinf(G.p.yaw), cy = cosf(G.p.yaw);
    float sp = sinf(G.p.pitch), cp = cosf(G.p.pitch);
    guVector cam = { cam_x, cam_y, cam_z };
    guVector up = { 0.0f, 1.0f, 0.0f };
    guVector look = { cam_x + cp * sy, cam_y + sp, cam_z - cp * cy };
    guLookAt(view, &cam, &up, &look);
    guPerspective(proj, 60.0f, aspect, 0.1f, 220.0f);

    /* 3D pass */
    GX_SetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
    GX_LoadPosMtxImm(view, GX_PNMTX0);
    draw_world(eye_y);

    /* weapon in camera space */
    if (G.state == ST_PLAYING) {
        guMtxIdentity(ident);
        GX_LoadPosMtxImm(ident, GX_PNMTX0);
        draw_gun();
    }

    /* HUD pass */
    guOrtho(ortho, 0, 479, 0, 639, 0, 300);
    GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
    guMtxIdentity(ident);
    GX_LoadPosMtxImm(ident, GX_PNMTX0);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    draw_hud(title, dead_t);

    GX_DrawDone();
    fbi ^= 1;
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(xfb[fbi], GX_TRUE);
    VIDEO_SetNextFramebuffer(xfb[fbi]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

/* ================= sound effects (synthesised, so no files are needed) ================= */
#define SFX_RATE 32000
enum { SFX_SHOT, SFX_SHOTGUN, SFX_HIT, SFX_HEAD, SFX_HURT, SFX_PICKUP, SFX_RELOAD, SFX_GROAN, SFX_ROAR, SFX_ZDIE, SFX_CLICK, SFX_COUNT };

typedef struct { s16 *buf; u32 bytes; } Sfx;
static Sfx sfx[SFX_COUNT];
static u32 noise_state = 0x2468ACE1u;

static float noise(void)
{
    noise_state ^= noise_state << 13;
    noise_state ^= noise_state >> 17;
    noise_state ^= noise_state << 5;
    return ((float)(noise_state & 0xFFFF) / 32768.0f) - 1.0f;
}

static void make_sfx(int id, float dur)
{
    u32 frames = ((u32)(dur * SFX_RATE) + 7) & ~7u;       /* multiple of 8 frames = 32 bytes */
    sfx[id].bytes = frames * 4;
    sfx[id].buf = (s16 *)memalign(32, sfx[id].bytes);
    if (!sfx[id].buf) { sfx[id].bytes = 0; return; }
    float lp = 0.0f, ph = 0.0f;
    for (u32 i = 0; i < frames; i++) {
        float t = (float)i / SFX_RATE;
        float n = noise();
        lp += 0.18f * (n - lp);
        float v = 0.0f;
        switch (id) {
        case SFX_SHOT:    v = (n * 0.8f + lp * 0.6f) * expf(-t * 26.0f) + sinf(2.0f * PI_F * 120.0f * t) * expf(-t * 22.0f) * 0.6f; break;
        case SFX_SHOTGUN: v = (n * 0.7f + lp * 1.1f) * expf(-t * 9.0f) + sinf(2.0f * PI_F * 70.0f * t) * expf(-t * 8.0f) * 0.9f; break;
        case SFX_HIT:     v = lp * 1.4f * expf(-t * 32.0f) + sinf(2.0f * PI_F * 150.0f * t) * expf(-t * 28.0f) * 0.5f; break;
        case SFX_HEAD:    v = sinf(2.0f * PI_F * (900.0f - t * 2500.0f) * t) * expf(-t * 24.0f) * 0.5f + lp * expf(-t * 30.0f); break;
        case SFX_HURT:    ph += 2.0f * PI_F * (220.0f - 260.0f * t) / SFX_RATE; v = sinf(ph) * expf(-t * 7.0f) * 0.7f + lp * 0.3f * expf(-t * 12.0f); break;
        case SFX_PICKUP:  ph += 2.0f * PI_F * (600.0f + 1400.0f * t) / SFX_RATE; v = sinf(ph) * (1.0f - t / dur) * 0.5f; break;
        case SFX_RELOAD:  v = n * expf(-t * 70.0f) * 0.8f + sinf(2.0f * PI_F * 1800.0f * t) * expf(-t * 90.0f) * 0.4f; break;
        case SFX_GROAN: {
            float f0 = 62.0f + 18.0f * sinf(t * 9.0f) + 25.0f * t;
            ph += 2.0f * PI_F * f0 / SFX_RATE;
            float saw = fmodf(ph, 2.0f * PI_F) / PI_F - 1.0f;
            v = (saw * 0.6f + lp * 0.5f) * sinf(PI_F * t / dur);
            break;
        }
        case SFX_ROAR: {
            float f0 = 48.0f + 14.0f * sinf(t * 5.0f);
            ph += 2.0f * PI_F * f0 / SFX_RATE;
            float saw = fmodf(ph, 2.0f * PI_F) / PI_F - 1.0f;
            v = (saw * 0.55f + lp * 0.9f) * sinf(PI_F * t / dur);
            break;
        }
        case SFX_ZDIE:    ph += 2.0f * PI_F * (110.0f - 150.0f * t) / SFX_RATE; v = (fmodf(ph, 2.0f * PI_F) / PI_F - 1.0f) * 0.5f * (1.0f - t / dur) + lp * 0.3f * expf(-t * 10.0f); break;
        case SFX_CLICK:   v = n * expf(-t * 120.0f) * 0.7f; break;
        default: break;
        }
        s16 s = (s16)(clampf(v, -1.0f, 1.0f) * 20000.0f);
        sfx[id].buf[2 * i] = s;
        sfx[id].buf[2 * i + 1] = s;
    }
    DCFlushRange(sfx[id].buf, sfx[id].bytes);
}

static void sfx_init(void)
{
    make_sfx(SFX_SHOT, 0.22f);
    make_sfx(SFX_SHOTGUN, 0.50f);
    make_sfx(SFX_HIT, 0.12f);
    make_sfx(SFX_HEAD, 0.18f);
    make_sfx(SFX_HURT, 0.30f);
    make_sfx(SFX_PICKUP, 0.20f);
    make_sfx(SFX_RELOAD, 0.12f);
    make_sfx(SFX_GROAN, 0.70f);
    make_sfx(SFX_ROAR, 1.20f);
    make_sfx(SFX_ZDIE, 0.40f);
    make_sfx(SFX_CLICK, 0.08f);
}

static void sfx_play(int id, int vol)
{
    if (!sfx[id].buf || sfx[id].bytes == 0) return;
    s32 v = ASND_GetFirstUnusedVoice();
    if (v < 0) return;
    ASND_SetVoice(v, VOICE_STEREO_16BIT, SFX_RATE, 0, sfx[id].buf, (s32)sfx[id].bytes, vol, vol, NULL);
}

static void play_events(const Events *e, float *rumble)
{
    if (e->shot)     { sfx_play(SFX_SHOT, 230); *rumble = 0.06f; }
    if (e->shotgun)  { sfx_play(SFX_SHOTGUN, 255); *rumble = 0.18f; }
    if (e->hit)      sfx_play(SFX_HIT, 200);
    if (e->headshot) sfx_play(SFX_HEAD, 230);
    if (e->hurt)     { sfx_play(SFX_HURT, 255); *rumble = 0.25f; }
    if (e->pickup)   sfx_play(SFX_PICKUP, 200);
    if (e->reload)   sfx_play(SFX_RELOAD, 200);
    if (e->click)    sfx_play(SFX_CLICK, 180);
    if (e->zdie)     sfx_play(SFX_ZDIE, 190);
    if (e->groan)    sfx_play(SFX_GROAN, 150);
    if (e->roar)     sfx_play(SFX_ROAR, 255);
}

/* ================= input ================= */
static float turn_curve(float v)
{
    float a = fabsf(v);
    const float dead = 0.55f;
    if (a < dead) return 0.0f;
    float t = clampf((a - dead) / (1.0f - dead), 0.0f, 1.0f);
    return v < 0.0f ? -t : t;
}

static void read_input(Input *in, int *a_down, int *home_down)
{
    memset(in, 0, sizeof(*in));
    WPAD_ScanPads();
    u32 down = WPAD_ButtonsDown(0), held = WPAD_ButtonsHeld(0);
    WPADData *wd = WPAD_Data(0);

    in->tan_y = tanf(30.0f * PI_F / 180.0f);
    in->tan_x = in->tan_y * aspect;

    if (wd && wd->ir.valid) {
        float tx = clampf((wd->ir.x - 320.0f) / 320.0f, -1.0f, 1.0f);
        float ty = clampf(-(wd->ir.y - 240.0f) / 240.0f, -1.0f, 1.0f);
        ptr_x += (tx - ptr_x) * 0.55f;
        ptr_y += (ty - ptr_y) * 0.55f;
    }
    in->aim_x = ptr_x;
    in->aim_y = ptr_y;
    in->yaw_rate = turn_curve(ptr_x) * 2.6f;
    in->pitch_rate = turn_curve(ptr_y) * 1.5f;

    int have_nunchuk = (wd && wd->exp.type == WPAD_EXP_NUNCHUK);
    if (have_nunchuk) {
        float mag = wd->exp.nunchuk.js.mag;
        float ang = wd->exp.nunchuk.js.ang * PI_F / 180.0f;
        if (mag > 0.15f) {
            in->move_f = clampf(mag, 0.0f, 1.0f) * cosf(ang);
            in->move_s = clampf(mag, 0.0f, 1.0f) * sinf(ang);
        }
        in->sprint = (held & WPAD_NUNCHUK_BUTTON_C) != 0;
        in->weapon_next = (down & (WPAD_NUNCHUK_BUTTON_Z | WPAD_BUTTON_UP)) != 0;
        in->weapon_prev = (down & WPAD_BUTTON_DOWN) != 0;
    } else {                               /* fallback without Nunchuk: D-pad moves */
        in->move_f = ((held & WPAD_BUTTON_RIGHT) ? 1.0f : 0.0f) - ((held & WPAD_BUTTON_LEFT) ? 1.0f : 0.0f);
        in->move_s = ((held & WPAD_BUTTON_DOWN) ? 1.0f : 0.0f) - ((held & WPAD_BUTTON_UP) ? 1.0f : 0.0f);
        in->sprint = (held & WPAD_BUTTON_1) != 0;
        in->weapon_next = (down & WPAD_BUTTON_PLUS) != 0;
        in->weapon_prev = (down & WPAD_BUTTON_MINUS) != 0;
    }
    in->fire = (held & WPAD_BUTTON_B) != 0;
    in->fire_pressed = (down & WPAD_BUTTON_B) != 0;
    in->reload = (down & WPAD_BUTTON_A) != 0;
    *a_down = (down & WPAD_BUTTON_A) != 0;
    *home_down = (down & WPAD_BUTTON_HOME) != 0;
}

/* ================= main ================= */
int main(void)
{
    video_init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);
    SYS_SetResetCallback(on_reset);
    SYS_SetPowerCallback(on_power);

    ASND_Init();
    sfx_init();
    ASND_Pause(0);

    game_reset(&G, (u32)gettime());
    int title = 1;
    float bob = 0.0f, dead_t = 0.0f, rumble_t = 0.0f;
    int rumbling = 0;
    u64 last = gettime();

    while (!quit_flag) {
        u64 now = gettime();
        float dt = (float)ticks_to_microsecs(now - last) / 1000000.0f;
        last = now;
        dt = clampf(dt, 0.001f, 0.05f);

        Input in;
        int a_down, home_down;
        read_input(&in, &a_down, &home_down);
        if (home_down) quit_flag = 1;

        if (title) {
            if (a_down) { title = 0; game_reset(&G, (u32)now); }
            memset(&in, 0, sizeof(in));
            in.tan_y = tanf(30.0f * PI_F / 180.0f);
            in.tan_x = in.tan_y * aspect;
            game_update(&G, &in, dt);        /* keeps particles etc. alive, player stands still */
        } else if (G.state != ST_PLAYING && a_down) {
            game_reset(&G, (u32)now);
            dead_t = 0.0f;
        } else {
            float ox = G.p.x, oz = G.p.z;
            game_update(&G, &in, dt);
            bob += sqrtf((G.p.x - ox) * (G.p.x - ox) + (G.p.z - oz) * (G.p.z - oz)) * 2.4f;
        }

        if (G.state == ST_DEAD) dead_t += dt; else dead_t = 0.0f;

        float rumble = 0.0f;
        play_events(&G.ev, &rumble);
        game_clear_events(&G);
        if (rumble > 0.0f) rumble_t = rumble;
        if (rumble_t > 0.0f) {
            rumble_t -= dt;
            if (!rumbling) { WPAD_Rumble(0, 1); rumbling = 1; }
        } else if (rumbling) {
            WPAD_Rumble(0, 0);
            rumbling = 0;
        }

        render_frame(title, bob, dead_t);
    }

    WPAD_Rumble(0, 0);
    ASND_Pause(1);
    ASND_End();
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    if (quit_flag == 2) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    return 0;
}
