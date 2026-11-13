/*
 * libFuzzer harness for the RP2354 <-> nRF54 packet link (nRF54/app/src/link.h)
 * and the UART receive-ring bookkeeping (nRF54/app/src/uartrx.h).
 *
 * byte 0 picks the mode:
 *   even - the rest is raw wire bytes, fed to the COBS/CRC-8 receiver; every
 *          packet that gets through is handed to the stream state machine.
 *   odd  - the rest is a script of operations (encode and receive a packet,
 *          send on a stream, ack, ask, tick, UART ring updates, raw bytes).
 *          Well-formed packets get past the CRC, so the stream state machine,
 *          flow control and eviction are reached, and every packet that is
 *          encoded is checked to decode back to what it was made from.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "nRF54/app/src/link.h"
#include "nRF54/app/src/uartrx.h"

static void need(bool ok)
{
	if (!ok)
		abort();
}

static struct link lk;

/* Feed bytes to the receiver and deal with every packet that comes out. */
static void feed(const uint8_t *p, size_t n, bool expect, const uint8_t *want,
		 size_t want_len)
{
	bool got = false;

	for (size_t i = 0; i < n; i++) {
		if (!link_rx_byte(&lk.rx, p[i]))
			continue;
		need(lk.rx.len >= LINK_HEADER && lk.rx.len <= LINK_FRAME_MAX - 1);
		if (expect) {
			need(i + 1 == n);
			need(lk.rx.len == want_len);
			need(memcmp(lk.rx.buf, want, want_len) == 0);
			got = true;
		}
		if (link_take(&lk) == LINK_GOT_DATA)
			link_consumed(&lk);
	}
	if (expect)
		need(got);
}

static void feed_out(const uint8_t *out, size_t n)
{
	need(n >= 2 && n <= LINK_WIRE_MAX);
	need(out[n - 1] == 0);
	for (size_t i = 0; i + 1 < n; i++)
		need(out[i] != 0);
	feed(out, n, false, NULL, 0);
}

static void script(const uint8_t *d, size_t n)
{
	struct uartrx ur;
	uint32_t now = 0;
	size_t i = 0;

	memset(&ur, 0, sizeof ur);
	link_init(&lk);
	/* the receiver believes nothing before its first zero */
	{
		const uint8_t z = 0;
		feed(&z, 1, false, NULL, 0);
	}

	while (i < n) {
		uint8_t op = d[i++] & 7;
		uint8_t out[LINK_WIRE_MAX];
		uint8_t pkt[LINK_FRAME_MAX];
		uint8_t a[4];
		size_t len;

		switch (op) {
		case 0: { /* encode a packet and receive it */
			if (n - i < 5)
				return;
			memcpy(a, d + i, 4);
			len = d[i + 4] % (LINK_PAYLOAD_MAX + 1);
			i += 5;
			if (len > n - i)
				len = n - i;
			size_t w = link_encode(a[0], a[1], a[2], a[3], d + i, len, out);
			memcpy(pkt, a, 4);
			memcpy(pkt + 4, d + i, len);
			i += len;
			need(w <= LINK_WIRE_MAX);
			need(out[w - 1] == 0);
			for (size_t k = 0; k + 1 < w; k++)
				need(out[k] != 0);
			feed(out, w, true, pkt, LINK_HEADER + len);
			break;
		}
		case 1: /* time passes */
			if (n - i < 2)
				return;
			now += (uint32_t)d[i] | (uint32_t)d[i + 1] << 8;
			i += 2;
			link_tick(&lk, now);
			break;
		case 2: { /* send on a stream */
			if (n - i < 4)
				return;
			memcpy(a, d + i, 4);
			i += 4;
			len = a[3] % (LINK_PAYLOAD_MAX + 1);
			if (len > n - i)
				len = n - i;
			if (link_can_send(&lk, a[0], a[1])) {
				size_t w = link_pack(&lk, a[0], a[1], a[2], d + i, len, out);
				need(w <= LINK_WIRE_MAX);
				need(out[w - 1] == 0);
			}
			i += len;
			break;
		}
		case 3: { /* an acknowledgement or ask, looped back */
			size_t w = link_pack_ack(&lk, out);
			if (w)
				feed_out(out, w);
			break;
		}
		case 4:
			link_ack_again(&lk);
			break;
		case 5: { /* raw wire bytes, then a zero to resynchronise */
			size_t c;
			const uint8_t z = 0;

			if (i >= n)
				return;
			c = d[i++];
			if (c > n - i)
				c = n - i;
			feed(d + i, c, false, NULL, 0);
			i += c;
			feed(&z, 1, false, NULL, 0);
			break;
		}
		case 6: { /* the receive DMA moves */
			uint32_t amount;

			if (n - i < 3)
				return;
			amount = (uint32_t)d[i + 1] | (uint32_t)d[i + 2] << 8;
			uartrx_written(&ur, d[i] & 1, amount & (UARTRX_SIZE | UARTRX_MASK));
			if (d[i] & 2)
				(void)uartrx_overrun(&ur);
			need((int32_t)(ur.head - ur.tail) >= 0 || ur.lost);
			i += 3;
			break;
		}
		case 7:
			if (n - i < 3)
				return;
			link_done(&lk, d[i], d[i + 1], d[i + 2]);
			i += 3;
			break;
		}
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1 || size > 4096)
		return -1;

	if (data[0] & 1) {
		script(data + 1, size - 1);
	} else {
		link_init(&lk);
		feed(data + 1, size - 1, false, NULL, 0);
	}
	return 0;
}
