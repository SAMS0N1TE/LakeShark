/* Docked walking rules. All layout storage belongs to this one active place. */
#include "walk.h"
#include <math.h>
#include <string.h>

#define WALK_RAD .01745329252f
#define WALK_PI 3.14159265359f

static walk_box boxes[WALK_BOXES];
static walk_spot spots[WALK_SPOTS];
static int box_count, spot_count;
static walk_place current_place;
static walk_hull hull;
/* The ramp box bounds its slope: high at -z, low at +z. Aboard, its
 * upper landing is level with the floor; USE crosses the place boundary. */
static float ramp_front, ramp_back, ramp_height, ramp_half_width;

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

static void box(int kind, bool solid, float x, float y, float z,
                float hx, float hy, float hz)
{
    if (box_count >= WALK_BOXES) return;
    walk_box *b = &boxes[box_count++];
    b->c = l3v(x, y, z);
    b->half = l3v(hx, hy, hz);
    b->kind = (uint8_t)kind;
    b->solid = solid;
}

static void spot(int kind, const char *label, float x, float z, float reach)
{
    if (spot_count >= WALK_SPOTS) return;
    walk_spot *s = &spots[spot_count++];
    s->p = l3v(x, 1, z);
    s->kind = (uint8_t)kind;
    s->reach = reach;
    for (int i = 0; i < 19 && label[i]; ++i) s->label[i] = label[i];
}

/* Full-height cross wall, with a 1.8 m opening on the centre aisle. */
static void doorway(float half_width, float z)
{
    float wing = (half_width - .9f) * .5f;
    box(WB_WALL, true, -.9f-wing, 1.2f, z, wing, 1.2f, .1f);
    box(WB_WALL, true,  .9f+wing, 1.2f, z, wing, 1.2f, .1f);
    box(WB_DOOR, false, 0, 2.2f, z, .9f, .2f, .12f);
}

static void hangar(const walk_ship *ship)
{
    float length = clampf(ship->hull_radius * 4, 9, 70);
    hull.centre = l3v(0, .8f, 0);
    hull.half_length = length * .5f;
    hull.half_width = length * .275f;
    hull.height = length * .3f;
    hull.scale = length * .5f;
    /* The normal bay is 44 x 52 x 14. Capital hulls get a larger berth,
     * keeping at least six metres alongside and eight at either end. */
    float hw = fmaxf(22, hull.half_width + 6);
    float hl = fmaxf(26, hull.half_length + 8);
    float height = fmaxf(14, hull.height + 3);
    box(WB_FLOOR, true, 0, -.15f, 0, hw+.3f, .15f, hl+.3f);
    box(WB_CEILING, true, 0, height+.15f, 0, hw+.3f, .15f, hl+.3f);
    box(WB_WALL, true, -hw-.15f, height/2, 0, .15f, height/2, hl);
    box(WB_WALL, true,  hw+.15f, height/2, 0, .15f, height/2, hl);
    box(WB_WALL, true, 0, height/2, -hl-.15f, hw, height/2, .15f);
    box(WB_WALL, true, 0, height/2,  hl+.15f, hw, height/2, .15f);
    /* At most 28 ribs and 14 lights, even at the 70 m hull length cap. */
    for (float z = -hl+3; z < hl-1; z += 6) {
        box(WB_RIB, true, -hw+.15f, height/2, z, .15f, height/2, .18f);
        box(WB_RIB, true,  hw-.15f, height/2, z, .15f, height/2, .18f);
        box(WB_LIGHT, false, 0, height-.25f, z, hw*.6f, .08f, .18f);
    }
    box(WB_PAD, false, 0, .025f, 0,
        hull.half_width+1, .025f, hull.half_length+1);
    for (int side = -1; side <= 1; side += 2) {
        for (int end = -1; end <= 1; end += 2)
            box(WB_CLAMP, true, side*(hull.half_width+.9f), .65f,
                end*hull.half_length*.55f, .25f, .65f, .3f);
        box(WB_HAZARD, false, side*(hull.half_width+1.5f), .015f,
            0, .18f, .015f, hull.half_length+1);
        for (int i = 0; i < 4; ++i)
            box(WB_CRATE, true, side*(hw-1), .6f, -9+i*3,
                .6f, .6f, .6f);
    }
    box(WB_TABLE, true, -hw+1.3f, .45f, hl-5, .7f, .45f, 1.2f);
    box(WB_CONSOLE, true, -hw+1.3f, 1.1f, hl-5, .5f, .2f, .4f);
    spot(WS_DESK, "STATION DESK", -hw+2.8f, hl-5, 1.8f);
    for (int i = 0; i < 3; ++i)
        box(WB_RIB, true, -hw+.6f, .45f+i*.55f, 8, .3f, .12f, 1.4f);
    spot(WS_ARMOURY, "ARMOURY", -hw+1.8f, 8, 1.8f);
    box(WB_CONSOLE, true, hw-1, .65f, hl-5, .6f, .65f, .7f);
    spot(WS_YARD, "SHIPYARD", hw-2.4f, hl-5, 1.8f);
    box(WB_DOOR, true, 0, 4, -hl+.08f, 5, 4, .15f);

    ramp_front = hull.half_length + .4f;
    ramp_back = ramp_front + 4;
    ramp_height = .8f;
    ramp_half_width = .9f;
    box(WB_RAMP, false, 0, ramp_height/2, (ramp_front+ramp_back)/2,
        ramp_half_width, ramp_height/2, 2);
    spot(WS_RAMP_UP, "UP THE RAMP", 0, ramp_back-.15f, 1.8f);
}

static void interior(const walk_ship *ship)
{
    int size = ship->hull_radius <= 4 ? 0 : (ship->hull_radius <= 9 ? 1 : 2);
    float hw = size == 0 ? 2.4f : (size == 1 ? 3.4f : 4.4f);
    float hl = size == 0 ? 4.5f : (size == 1 ? 8 : 12);
    float hold_front = -hl + (size == 0 ? 3 : (size == 1 ? 5 : 8));
    bool engines = size > 0 && ship->has_engines_room;
    bool cabin = size == 2 && ship->seats > 0;
    float engine_front = hl - (size == 1 ? 3 : 4);
    box(WB_FLOOR, true, 0, -.1f, 0, hw+.2f, .1f, hl+.2f);
    box(WB_CEILING, true, 0, 2.5f, 0, hw+.2f, .1f, hl+.2f);
    box(WB_WALL, true, -hw-.1f, 1.2f, 0, .1f, 1.2f, hl);
    box(WB_WALL, true,  hw+.1f, 1.2f, 0, .1f, 1.2f, hl);
    box(WB_WINDOW, true, 0, 1.2f, -hl-.1f, hw, 1.2f, .1f);
    box(WB_WALL, true, 0, 1.2f, hl+.1f, hw, 1.2f, .1f);
    box(WB_SEAT, true, -1.45f, .55f, -hl+2.1f, .38f, .55f, .4f);
    box(WB_CONSOLE, true, 0, .65f, -hl+.5f, 1, .65f, .25f);
    spot(WS_SEAT, "SIT DOWN", 0, -hl+2.1f, 1.1f);
    spot(WS_CONSOLE, "SHIP'S CONSOLE", 0, -hl+1.25f, .8f);
    doorway(hw, -hl+3);
    if (size > 0) {
        doorway(hw, hold_front);
        float front = -hl+3, back = hold_front;
        box(WB_WALL, true, 1, 1.2f, (front+back)/2, .1f, 1.2f, (back-front)/2);
        if (cabin) {
            float mid = (front+back)/2;
            /* Cabin entrance in the port corridor wall. */
            box(WB_WALL, true, -1, 1.2f, (front+mid-.9f)/2,
                .1f, 1.2f, (mid-.9f-front)/2);
            box(WB_WALL, true, -1, 1.2f, (back+mid+.9f)/2,
                .1f, 1.2f, (back-mid-.9f)/2);
            box(WB_DOOR, false, -1, 2.2f, mid, .12f, .2f, .9f);
            int bunks = ship->seats > 8 ? 8 : ship->seats;
            for (int i = 0; i < bunks; ++i)
                box(WB_BUNK, true, -hw+.55f, .35f+(i%2)*1.05f,
                    front+.7f+(i/2)*1.15f, .45f, .15f, .45f);
            spot(WS_CABIN, "CABIN", -2, mid, 1.2f);
        } else {
            box(WB_WALL, true, -1, 1.2f, (front+back)/2,
                .1f, 1.2f, (back-front)/2);
        }
    }
    if (engines) {
        float mid = (engine_front+hl)/2;
        box(WB_WALL, true, 1, 1.2f, (engine_front+mid-.9f)/2,
            .1f, 1.2f, (mid-.9f-engine_front)/2);
        box(WB_WALL, true, 1, 1.2f, (hl+mid+.9f)/2,
            .1f, 1.2f, (hl-mid-.9f)/2);
        /* Close the front of this side room, leaving the hold aisle free. */
        box(WB_WALL, true, (1+hw)/2, 1.2f, engine_front,
            (hw-1)/2, 1.2f, .1f);
        box(WB_DOOR, false, 1, 2.2f, mid, .12f, .2f, .9f);
        for (int i = 0; i < 2; ++i)
            box(WB_ENGINE, true, hw-.6f, .7f,
                i ? hl-.7f : engine_front+.7f, .4f, .7f, .4f);
        box(WB_PIPE, true, hw-.3f, 2.1f, mid, .12f, .12f, (hl-engine_front)/2);
        spot(WS_ENGINES, "ENGINES", 1.65f, mid, 1.2f);
    }
    /* Two layers of small crates against the hold walls, never in the
     * aisle or the engine room. Count slots first, then fill proportionally. */
    int slots = 0;
    for (float z = hold_front+.8f; z <= hl-.8f; z += 1)
        for (int side = -1; side <= 1; side += 2)
            if (side < 0 || !engines || z < engine_front-.7f) ++slots;
    int maximum = slots*2 < 24 ? slots*2 : 24;
    int count = 0;
    if (ship->cargo_capacity > 0 && ship->cargo_used > 0) {
        float fraction = clampf((float)ship->cargo_used / ship->cargo_capacity, 0, 1);
        count = (int)ceilf(fraction * maximum);
    }
    for (int layer = 0; layer < 2 && count > 0; ++layer)
        for (float z = hold_front+.8f; z <= hl-.8f && count > 0; z += 1)
            for (int side = -1; side <= 1 && count > 0; side += 2)
                if (side < 0 || !engines || z < engine_front-.7f) {
                    box(WB_CRATE, true, side*(hw-.5f), .35f+layer*.7f,
                        z, .35f, .35f, .35f);
                    --count;
                }
    spot(WS_HOLD, "THE HOLD", 0, (hold_front+hl)/2, 1.4f);
    ramp_front = hl-1.5f;
    ramp_back = hl-.5f;
    ramp_half_width = .8f;
    ramp_height = 0;
    box(WB_RAMP, false, 0, -.025f, hl-1, .8f, .025f, .5f);
    spot(WS_RAMP_DOWN, "DOWN THE RAMP", 0, hl-.7f, 1.2f);
}

static float floor_y(float x, float z)
{
    if (fabsf(x) <= ramp_half_width && z >= ramp_front && z <= ramp_back)
        return ramp_height * (ramp_back-z) / (ramp_back-ramp_front);
    return 0;
}

static bool clear_at(float x, float z)
{
    float y = floor_y(x, z);
    const float radius = WALK_BODY + .0001f;
    for (int i = 0; i < box_count; ++i) {
        const walk_box *b = &boxes[i];
        if (!b->solid || b->c.y+b->half.y <= y+.00001f ||
            b->c.y-b->half.y >= y+1.8f) continue;
        float dx = fmaxf(fabsf(x-b->c.x)-b->half.x, 0);
        float dz = fmaxf(fabsf(z-b->c.z)-b->half.z, 0);
        if (dx*dx + dz*dz < radius*radius) return false;
    }
    if (current_place == WALK_HANGAR) {
        float a = hull.half_width, b = hull.half_length;
        float px = fabsf(x-hull.centre.x), pz = fabsf(z-hull.centre.z);
        if (px < a+radius && pz < b+radius) {
            float aa = a*a, bb = b*b;
            if (px*px/aa + pz*pz/bb <= 1) return false;
            /* Closest point on the ellipse, using its Lagrange multiplier.
             * The low bound keeps the distance estimate conservative. */
            float lo = 0, hi = fmaxf(a, b)*sqrtf(px*px+pz*pz);
            for (int i = 0; i < 24; ++i) {
                float t = (lo+hi)*.5f;
                float ex = a*px/(t+aa), ez = b*pz/(t+bb);
                if (ex*ex+ez*ez > 1) lo = t; else hi = t;
            }
            float dx = px-aa*px/(lo+aa), dz = pz-bb*pz/(lo+bb);
            if (dx*dx+dz*dz < radius*radius) return false;
        }
    }
    return true;
}

static void find_spot(walk_state *w)
{
    float fx = sinf(w->yaw*WALK_RAD), fz = -cosf(w->yaw*WALK_RAD);
    float nearest = 1e30f;
    w->spot = -1;
    for (int i = 0; i < spot_count; ++i) {
        float dx = spots[i].p.x-w->feet.x, dz = spots[i].p.z-w->feet.z;
        float d2 = dx*dx+dz*dz;
        if (d2 <= spots[i].reach*spots[i].reach && d2 < nearest &&
            (d2 < .000001f || fx*dx+fz*dz >= .5f*sqrtf(d2))) {
            nearest = d2;
            w->spot = i;
        }
    }
}

void walk_enter(walk_state *w, walk_place place, const walk_ship *ship)
{
    memset(boxes, 0, sizeof(boxes));
    memset(spots, 0, sizeof(spots));
    memset(&hull, 0, sizeof(hull));
    memset(w, 0, sizeof(*w));
    box_count = spot_count = 0;
    current_place = w->place = place;
    if (place == WALK_HANGAR) {
        hangar(ship);
        w->feet = l3v(0, 0, ramp_back+.65f);
    } else {
        interior(ship);
        const float hl = ship->hull_radius <= 4 ? 4.5f :
                         (ship->hull_radius <= 9 ? 8 : 12);
        w->feet = l3v(0, 0, -hl+1.35f);
    }
    w->yaw = 180;
    find_spot(w);
}

int walk_boxes(const walk_box **out) { *out = boxes; return box_count; }
int walk_spots(const walk_spot **out) { *out = spots; return spot_count; }
const walk_hull *walk_hull_on_pad(void)
{
    return current_place == WALK_HANGAR ? &hull : NULL;
}

/* Reject a blocked axis, retaining as much travel as fits before contact.
 * Resolving the other axis separately lets the walker slide along it. */
static float move_axis(float x, float z, float delta, bool along_x)
{
    if (clear_at(x+(along_x ? delta : 0), z+(along_x ? 0 : delta))) return delta;
    float lo = 0, hi = 1;
    for (int i = 0; i < 12; ++i) {
        float t = (lo+hi)*.5f;
        if (clear_at(x+(along_x ? delta*t : 0), z+(along_x ? 0 : delta*t))) lo = t;
        else hi = t;
    }
    return delta*lo;
}

void walk_step(walk_state *w, float fwd, float strafe, float turn_deg, float look_deg, float dt)
{
    w->yaw += turn_deg;
    w->pitch = clampf(w->pitch+look_deg, -70, 70);
    fwd = clampf(fwd, -1, 1);
    strafe = clampf(strafe, -1, 1);
    float magnitude = sqrtf(fwd*fwd+strafe*strafe);
    if (magnitude > 1) { fwd /= magnitude; strafe /= magnitude; }
    float sn = sinf(w->yaw*WALK_RAD), cs = cosf(w->yaw*WALK_RAD);
    float dx = (sn*fwd+cs*strafe)*1.6f*fmaxf(dt, 0);
    float dz = (-cs*fwd+sn*strafe)*1.6f*fmaxf(dt, 0);
    /* No substep travels further than 4 cm, including at dt = 0.1 s. */
    int steps = (int)ceilf(sqrtf(dx*dx+dz*dz)/.04f);
    w->moved = 0;
    if (steps > 0) {
        dx /= steps; dz /= steps;
        for (int i = 0; i < steps; ++i) {
            float mx = move_axis(w->feet.x, w->feet.z, dx, true);
            w->feet.x += mx;
            float mz = move_axis(w->feet.x, w->feet.z, dz, false);
            w->feet.z += mz;
            w->moved += sqrtf(mx*mx+mz*mz);
        }
    }
    w->feet.y = floor_y(w->feet.x, w->feet.z);
    w->bob = fmodf(w->bob+w->moved/1.4f, 1);
    find_spot(w);
}

int walk_use(walk_state *w, const walk_ship *ship)
{
    find_spot(w);
    if (w->spot < 0) return -1;
    int kind = spots[w->spot].kind;
    if (kind == WS_RAMP_UP) {
        walk_enter(w, WALK_SHIP, ship);
        w->feet = l3v(0, 0, ramp_front+.2f);
        w->yaw = 0;
        find_spot(w);
    } else if (kind == WS_RAMP_DOWN) {
        walk_enter(w, WALK_HANGAR, ship);
    }
    return kind;
}

l3_vec walk_eye(const walk_state *w)
{
    return l3_add(w->feet, l3v(0, WALK_EYE+.025f*sinf(2*WALK_PI*w->bob), 0));
}
