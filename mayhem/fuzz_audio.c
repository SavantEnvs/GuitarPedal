/*
 * libFuzzer harness for the pedal's audio core, built for the host the way
 * Validation/bench/bench.c does it: the real Audio/effect.h, the effects in Effects/ and
 * single_sample(), with the two DMA/timer register blocks shimmed by
 * Validation/bench/shim/.  The include list and order are bench.c's.
 *
 * Input: byte 0 = how many effects to route (1..MAX_ROUTED_EFFECTS), then that
 * many effect-id bytes; byte = number of pot/mix settings, each 3 bytes
 * (effect, pot-or-mix selector, value); the rest is stereo audio, 8 bytes a
 * frame (two raw 32-bit samples), followed by a tail of silent frames.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SSE__
#include <xmmintrin.h>
#endif

/*
 * Every writable global this file defines or includes from the firmware (the effects' static
 * buffers and structs, the DMA buffer, the meters, the shim registers) goes into two sections,
 * and LLVMFuzzerTestOneInput puts them back to their start-up contents before each input, so a
 * result depends on that input alone.  The harness's own bookkeeping is defined after the
 * region is closed.
 */
#pragma clang section bss="fzbss" data="fzdata"

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/timer.h"

struct bench_dma bench_dma_regs;
struct bench_timer bench_timer_regs;

#include "status.h"

#include "Audio/types.h"
#include "Audio/util.h"
#include "Audio/envelope.h"
#include "Audio/single-pole.h"
#include "Audio/biquad.h"
#include "Audio/fft.h"
#include "Audio/analyze.h"

static volatile int user_interaction = 0;

#include "Audio/effect.h"

uint8_t effect_chain[MAX_ROUTED_EFFECTS];
uint8_t routed_effect_count = 0;

#include "effect-state.h"

#pragma clang section bss="" data=""

extern char __start_fzdata[], __stop_fzdata[], __start_fzbss[], __stop_fzbss[];

static unsigned char *fz_data_init;

/* Plain volatile byte loops: ASan's memcpy/memset would trip on the redzones between globals. */
__attribute__((no_sanitize("address", "undefined")))
static void fz_reset_state(void)
{
	volatile unsigned char *d = (volatile unsigned char *)__start_fzdata;
	size_t n = (size_t)(__stop_fzdata - __start_fzdata);

	if (!fz_data_init) {
		fz_data_init = malloc(n);
		for (size_t k = 0; k < n; k++)
			fz_data_init[k] = d[k];
	}
	for (size_t k = 0; k < n; k++)
		d[k] = fz_data_init[k];

	volatile unsigned char *b = (volatile unsigned char *)__start_fzbss;
	size_t m = (size_t)(__stop_fzbss - __start_fzbss);

	for (size_t k = 0; k < m; k++)
		b[k] = 0;
}

sample_t get_usb_audio_input(void)
{
	sample_t zero = { 0.0f, 0.0f };
	return zero;
}

sample_t get_radio_audio_input(void)
{
	sample_t zero = { 0.0f, 0.0f };
	return zero;
}

void put_radio_audio_output(raw_sample_t wet, raw_sample_t dry)
{
}

#define TAIL_FRAMES 128
#define MAX_FRAMES 512

static void bench_sample(raw_sample_t in, raw_sample_t *out)
{
	unsigned slot = cpu_idx;

	i2s_dma_buf[slot] = in;
	bench_dma_regs.ch[dma_rx].write_addr = (uintptr_t)&i2s_dma_buf[(slot + 1) & 15];
	bench_dma_regs.ch[dma_tx].read_addr  = (uintptr_t)&i2s_dma_buf[(slot + 8) & 15];
	bench_timer_regs.timerawl += 21;

	single_sample(1.0f);

	*out = i2s_dma_buf[slot];
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
#ifdef __SSE__
	_mm_setcsr(_mm_getcsr() | 0x8040);	/* FTZ | DAZ, as the pedal does */
#endif
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	routing_bitmap_t routable;
	size_t i = 0;
	unsigned n_route, n_set;

	if (size < 3 || size > 4096)
		return -1;

	fz_reset_state();

	for (int k = 0; k < (int)ARRAY_SIZE(effects); k++)
		reset_effect(effects[k]);

	n_route = 1 + data[i++] % MAX_ROUTED_EFFECTS;
	if (size - i < n_route + 1)
		return -1;

	routable = routing_start();
	for (unsigned k = 0; k < n_route; k++)
		(void)routing_add(&routable, data[i++] % EFFECT_COUNT);
	routing_end(routable);

	n_set = data[i++] % 16;
	for (unsigned k = 0; k < n_set && size - i >= 3; k++, i += 3) {
		struct effect *e = effects[data[i] % EFFECT_COUNT];
		unsigned sel = data[i + 1] % 11;

		if (sel == 10) {
			set_effect_mix(e, data[i + 2] % 121);
		} else if (e->pots[sel].label) {
			int mx = max_pot_val(e, sel);

			set_effect_pot(e, sel, mx > 0 ? data[i + 2] % (mx + 1) : 0);
		}
	}

	for (int k = 0; k < (int)ARRAY_SIZE(effects); k++) {
		struct effect *e = effects[k];

		e->last = e->seq;
		if (e->prepare)
			e->prepare(effect_pots(e));
		if (e->init)
			e->init(effect_pots(e));
	}
	init_meters();

	for (unsigned f = 0; f < MAX_FRAMES + TAIL_FRAMES; f++) {
		raw_sample_t in, out;

		if (size - i >= 8 && f < MAX_FRAMES) {
			int32_t l, r;

			memcpy(&l, data + i, 4);
			memcpy(&r, data + i + 4, 4);
			i += 8;
			in.left = l;
			in.right = r;
		} else if (f < MAX_FRAMES) {
			f = MAX_FRAMES - 1;
			continue;
		} else {
			in.left = 0;
			in.right = 0;
		}
		bench_sample(in, &out);
	}
	return 0;
}
