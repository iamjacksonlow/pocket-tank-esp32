/* audio_host.c - host check of the sound mixer (common/audio.c) against the
 * real bank. Build + run from firmware/host_test:
 *   cc -O1 -std=c11 -I../../common -o audio_host audio_host.c ../../common/audio.c ../../common/sounds.c && ./audio_host
 * Checks: a deferred cue is silent; cooldowns and the per-second cap drop
 * extra starts; night mutes tier 2 and dims tier 1; volume off is silence;
 * a loop keeps going past its length and audio_stop fades it to nothing;
 * a single cue at normal volume never clips. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"

static int16_t *bank;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int peak_of(int16_t *buf, int n) { int p = 0; for (int i = 0; i < n; i++) { int a = abs(buf[i]); if (a > p) p = a; } return p; }
static int render_all(int16_t *buf, int n) { return audio_render(buf, n); }

int main(void) {
    FILE *f = fopen("../../assets/sounds/sounds.bin", "rb");
    if (!f) { printf("no bank (tools/make_sounds.py build)\n"); return 1; }
    bank = malloc(SND_BANK_BYTES);
    if (fread(bank, 1, SND_BANK_BYTES, f) != SND_BANK_BYTES) { printf("bank size mismatch\n"); return 1; }
    fclose(f);
    static int16_t buf[16000 * 4];
    audio_init(bank, SND_BANK_SAMPLES);
    audio_set_volume(2);

    CHECK(!audio_play(SND_TAP, AUDIO_PITCH_ONE, 1000), "a deferred cue (tap) started");
    CHECK(audio_play(SND_EAT, AUDIO_PITCH_ONE, 1000), "eat did not start");
    CHECK(!audio_play(SND_EAT, AUDIO_PITCH_ONE, 1100), "eat restarted inside its 250 ms cooldown");
    CHECK(audio_play(SND_EAT, 320, 1300), "eat did not restart after the cooldown");
    int live = render_all(buf, 16000);
    CHECK(live == 0, "eat still live after a second (%d)", live);
    int pk = peak_of(buf, 16000);
    CHECK(pk > 1000 && pk < 32767, "eat peak %d (expected audible, unclipped)", pk);

    /* the per-second cap: 6 starts, the 7th dropped */
    int started = 0;
    static const int cues[7] = { SND_FEED, SND_LIGHT_ON, SND_LIGHT_OFF, SND_SNIP, SND_CARD_OPEN, SND_WHEEL_TICK, SND_CONFIRM };
    for (int i = 0; i < 7; i++) started += audio_play(cues[i], AUDIO_PITCH_ONE, 5000 + i * 10);
    CHECK(started == 6, "cap: %d of 7 started (want 6)", started);
    audio_stop_all();

    /* night: tier 2 muted, tier 1 plays quieter */
    audio_set_night(false);
    audio_play(SND_FEED, AUDIO_PITCH_ONE, 9000); render_all(buf, 8000); int day = peak_of(buf, 8000);
    audio_set_night(true);
    CHECK(!audio_play(SND_EAT, AUDIO_PITCH_ONE, 9500), "eat played at night");
    audio_play(SND_FEED, AUDIO_PITCH_ONE, 10000); render_all(buf, 8000); int night = peak_of(buf, 8000);
    CHECK(night > 0 && night < day / 2, "night feed %d vs day %d (want about a quarter)", night, day);
    audio_set_night(false);

    /* volume off = silence */
    audio_set_volume(0);
    CHECK(!audio_play(SND_FEED, AUDIO_PITCH_ONE, 12000), "feed started with the volume off");
    audio_set_volume(2);

    /* the loop: past its own length it is still live; stop fades it out */
    CHECK(audio_play(SND_BUBBLES_LOOP, AUDIO_PITCH_ONE, 20000), "loop did not start");
    CHECK(!audio_play(SND_BUBBLES_LOOP, AUDIO_PITCH_ONE, 20001), "a second loop instance started");
    live = render_all(buf, 16000 * 3);
    CHECK(live == 1, "loop not live after 3 s (%d)", live);
    int seam_pk = peak_of(buf, 16000 * 3);
    CHECK(seam_pk > 500, "loop is silent (%d)", seam_pk);
    audio_stop(SND_BUBBLES_LOOP);
    live = render_all(buf, 1600);                         /* 100 ms > the 50 ms fade */
    CHECK(live == 0, "loop still live 100 ms after stop (%d)", live);
    CHECK(peak_of(buf + 1200, 400) == 0, "loop not silent after its fade");

    /* the biggest cue alone never clips */
    for (int c = 0; c < SND_COUNT; c++) {
        if (!SND_CUES[c].n_var || SND_CUES[c].loop) continue;
        audio_stop_all(); audio_play(c, AUDIO_PITCH_ONE, 30000 + c * 4000);
        render_all(buf, 16000 * 4);
        int p = peak_of(buf, 16000 * 4);
        CHECK(p < 32767, "%s clips (%d)", SND_CUES[c].name, p);
        CHECK(p > 0, "%s is silent", SND_CUES[c].name);
    }
    if (fails) printf("audio_host: %d FAILED\n", fails);
    else printf("audio_host: all checks passed (%u cues, bank %u KB)\n", (unsigned)SND_COUNT, (unsigned)(SND_BANK_BYTES / 1024));
    return fails != 0;
}
