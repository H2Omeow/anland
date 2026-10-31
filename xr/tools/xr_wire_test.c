/* xr_wire_test.c — host-side self test for the XR wire format.
 *
 * The XR path normally needs a phone, a headset, SteamVR and a root daemon to
 * exercise anything. The one layer that can be checked anywhere is the contract
 * between the daemon and the OpenVR driver: anland-xr-display is text, the pose
 * and input files are seqlock blocks. This test round-trips all three, so a
 * change to anland_xr_wire.h that breaks the driver fails in CI instead of on a
 * headset.
 *
 * Build + run:  xr/build.sh --test      (plain cc, no Vulkan, no Android)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "anland_xr_wire.h"

static int g_fail;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

static int nearly(double a, double b) {
    const double d = a - b;
    return (d < 0 ? -d : d) < 1e-6;
}

/* ---- display file: text, written by the daemon, polled by the driver ---- */
static void test_display(void) {
    struct anland_xr_display out, in;
    anland_xr_display_defaults(&out);
    out.version = ANLAND_XR_WIRE_VERSION;
    out.mode = ANLAND_XR_MODE_SBS_H;
    out.eye_width = 1832;
    out.eye_height = 1920;
    out.refresh_hz = 90.0;
    out.fov_left = -1.25;
    out.fov_right = 1.25;
    out.fov_top = 1.10;
    out.fov_bottom = -1.10;
    out.ipd = 0.063;
    out.render_scale = 0.9;

    char path[] = "/tmp/anland-xr-display-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp");
    if (fd < 0) return;
    FILE* f = fdopen(fd, "w+");
    CHECK(f != NULL, "fdopen");
    if (!f) { close(fd); unlink(path); return; }
    anland_xr_display_write(f, &out);
    fflush(f);
    rewind(f);

    char text[2048] = "";
    const size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    unlink(path);

    CHECK(got > 0, "empty display file");
    memset(&in, 0xAA, sizeof(in));
    const int rc = anland_xr_display_parse(text, &in);
    CHECK(rc == 1, "display parse returned %d (1 = ok)", rc);
    CHECK(in.version == out.version, "version %u != %u", in.version, out.version);
    CHECK(in.mode == out.mode, "mode %u != %u", in.mode, out.mode);
    CHECK(in.eye_width == out.eye_width, "eye_width %u != %u", in.eye_width, out.eye_width);
    CHECK(in.eye_height == out.eye_height, "eye_height %u != %u", in.eye_height,
          out.eye_height);
    CHECK(nearly(in.refresh_hz, out.refresh_hz), "refresh %.3f != %.3f", in.refresh_hz,
          out.refresh_hz);
    CHECK(nearly(in.fov_left, out.fov_left), "fov_left");
    CHECK(nearly(in.fov_right, out.fov_right), "fov_right");
    CHECK(nearly(in.fov_top, out.fov_top), "fov_top");
    CHECK(nearly(in.fov_bottom, out.fov_bottom), "fov_bottom");
    CHECK(nearly(in.ipd, out.ipd), "ipd %.6f != %.6f", in.ipd, out.ipd);
    CHECK(nearly(in.render_scale, out.render_scale), "render_scale");

    /* a missing key must keep the caller's value, not zero the struct */
    struct anland_xr_display part;
    anland_xr_display_defaults(&part);
    part.ipd = 0.099;
    /* a line without the version/eye-size keys is still applied, but parse
     * reports 0 ("nothing usable announced") so the driver keeps its defaults */
    CHECK(anland_xr_display_parse("# only a comment\nmode=3\n", &part) == 0,
          "a partial block must not claim to be usable");
    CHECK(part.mode == ANLAND_XR_MODE_SBS_V, "partial mode");
    CHECK(nearly(part.ipd, 0.099), "partial parse clobbered ipd");

    /* mode=0 is the "no XR session" marker the layer polls for */
    struct anland_xr_display off;
    anland_xr_display_defaults(&off);
    CHECK(anland_xr_display_parse("mode=0\n", &off) == 0,
          "mode=0 alone carries no version/eye size, so parse must report 0");
    CHECK(off.mode == ANLAND_XR_MODE_OFF, "mode=0 became %u", off.mode);
}

/* ---- pose / input blocks: seqlock, written by the daemon, mmapped read-only
 *      by the driver ---- */
static void test_pose_block(void) {
    struct anland_xr_pose_block* blk = calloc(1, ANLAND_XR_BLOCK_BYTES);
    CHECK(blk != NULL, "calloc pose block");
    if (!blk) return;

    /* exactly what awl_xr.cpp's publish_pose_locked() does */
    volatile uint32_t* seq = (volatile uint32_t*)&blk->seq;
    anland_xr_seq_begin(seq);
    blk->magic = ANLAND_XR_POSE_MAGIC;
    blk->pose.sample_ns = 1234567890ull;
    blk->pose.period_ns = 11111111ull;
    blk->pose.quat[0] = 0.0;
    blk->pose.quat[1] = 0.3826834; /* 45° about y */
    blk->pose.quat[2] = 0.0;
    blk->pose.quat[3] = 0.9238795;
    blk->pose.pos[0] = 0.1;
    blk->pose.pos[1] = 1.6;
    blk->pose.pos[2] = -0.2;
    blk->pose.ang_vel[0] = 0.5;
    blk->pose.ang_vel[1] = -0.25;
    blk->pose.ang_vel[2] = 0.125;
    blk->pose.ipd = 0.063;
    blk->pose.tracking = 1;
    anland_xr_seq_end(seq);

    CHECK(blk->seq == 2, "seq should be 2 after one write, is %u", blk->seq);
    CHECK((blk->seq & 1u) == 0, "seq must be even between writes");

    /* the driver's reader, reduced to the checks that matter */
    const uint32_t before = anland_xr_seq_read((volatile uint32_t*)&blk->seq);
    struct anland_xr_pose p;
    memcpy(&p, &blk->pose, sizeof(p));
    const uint32_t after = anland_xr_seq_read((volatile uint32_t*)&blk->seq);
    CHECK(anland_xr_seq_stable(before, after), "stable read failed");
    CHECK(blk->magic == ANLAND_XR_POSE_MAGIC, "magic mismatch");
    CHECK(p.sample_ns == 1234567890ull, "sample_ns");
    CHECK(p.tracking == 1, "tracking flag");
    CHECK(nearly(p.pos[1], 1.6), "pos y");
    CHECK(nearly(p.ang_vel[0], 0.5), "ang_vel x — driver prediction reads this");

    /* a torn write must be visible as odd/!stable, never silently accepted */
    anland_xr_seq_begin(seq);
    CHECK((anland_xr_seq_read(seq) & 1u) == 1, "seq must be odd mid-write");
    const uint32_t mid = anland_xr_seq_read(seq);
    anland_xr_seq_end(seq);
    CHECK(!anland_xr_seq_stable(mid, anland_xr_seq_read(seq)),
          "a read across a write must not look stable");
    CHECK((anland_xr_seq_read(seq) & 1u) == 0, "seq must be even after a write");

    free(blk);
}

static void test_input_block(void) {
    struct anland_xr_input_block* blk = calloc(1, ANLAND_XR_BLOCK_BYTES);
    CHECK(blk != NULL, "calloc input block");
    if (!blk) return;
    CHECK(sizeof(struct anland_xr_input) + 2 * sizeof(uint32_t) <= ANLAND_XR_BLOCK_BYTES,
          "input structs do not fit in one block");
    CHECK(sizeof(struct anland_xr_pose) + 2 * sizeof(uint32_t) <= ANLAND_XR_BLOCK_BYTES,
          "pose structs do not fit in one block");

    volatile uint32_t* seq = (volatile uint32_t*)&blk->seq;
    anland_xr_seq_begin(seq);
    blk->magic = ANLAND_XR_INPUT_MAGIC;
    blk->input.n = 2;
    blk->input.sample_ns = 42;
    blk->input.c[0].present = 1;
    blk->input.c[0].buttons = ANLAND_XR_BTN_TRIGGER_CLICK | ANLAND_XR_BTN_A;
    blk->input.c[0].trigger = 0.75f;
    blk->input.c[0].quat[3] = 1.0;
    blk->input.c[1].present = 1;
    blk->input.c[1].buttons = ANLAND_XR_BTN_GRIP_CLICK;
    blk->input.c[1].stick_x = -0.5f;
    blk->input.c[1].quat[3] = 1.0;
    anland_xr_seq_end(seq);

    CHECK(blk->magic == ANLAND_XR_INPUT_MAGIC, "input magic");
    CHECK(blk->input.n == 2, "input n");
    CHECK(blk->input.c[0].buttons & ANLAND_XR_BTN_A, "A bit");
    CHECK(nearly(blk->input.c[0].trigger, 0.75), "trigger");
    CHECK(nearly(blk->input.c[1].stick_x, -0.5), "stick x");
    CHECK(blk->input.c[1].quat[3] == 1.0, "untracked controller stays identity");
    free(blk);
}

int main(void) {
    printf("anland xr wire test (version %d)\n", ANLAND_XR_WIRE_VERSION);
    test_display();
    test_pose_block();
    test_input_block();
    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
