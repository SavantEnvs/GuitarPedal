/*
 * Known-answer probes for the link and MIDI parsers, run by mayhem/test.sh.
 * Each line is compared there against a value worked out by hand (COBS and
 * CRC-8/0x07 done independently of link.h).
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nRF54/app/src/link.h"
#include "Firmware/midi/midi.h"

static void hex(const char *tag, const uint8_t *p, size_t n)
{
	printf("%s", tag);
	for (size_t i = 0; i < n; i++)
		printf(" %02x", p[i]);
	printf("\n");
}

int main(void)
{
	uint8_t out[LINK_WIRE_MAX];
	const uint8_t note[] = { 0x90, 0x3c, 0x64 };
	const uint8_t zeros[] = { 0x00, 0x01, 0x02, 0x00, 0xff };
	size_t n;
	struct link_rx rx;
	struct midi_parser mp;
	uint8_t pk[4];
	const uint8_t stream[] = { 0x90, 0x3c, 0x64, 0x3d, 0x65, 0xc1, 0x07, 0xf8 };
	int got = 0;

	n = link_encode(LINK_MIDI, 0, 0, LINK_FIRST | LINK_LAST, note, sizeof note, out);
	hex("KAT link_encode midi:", out, n);
	n = link_encode(LINK_ACK, 1, 5, 0, zeros, sizeof zeros, out);
	hex("KAT link_encode zeros:", out, n);

	/* decode it again, through a receiver that has seen its first zero */
	link_rx_init(&rx);
	link_rx_byte(&rx, 0);
	for (size_t i = 0; i < n; i++)
		if (link_rx_byte(&rx, out[i]))
			hex("KAT link_rx:", rx.buf, rx.len);

	/* one bit wrong: the CRC must refuse it */
	n = link_encode(LINK_MIDI, 0, 0, LINK_FIRST | LINK_LAST, note, sizeof note, out);
	out[5] ^= 0x01;
	link_rx_init(&rx);
	link_rx_byte(&rx, 0);
	for (size_t i = 0; i < n; i++)
		if (link_rx_byte(&rx, out[i]))
			printf("KAT link_crc: accepted\n");
	printf("KAT link_crc: dropped %u\n", (unsigned)rx.crc);

	memset(&mp, 0, sizeof mp);
	for (size_t i = 0; i < sizeof stream; i++)
		if (midi_parse_byte(&mp, stream[i], pk)) {
			char tag[32];
			snprintf(tag, sizeof tag, "KAT midi %d:", ++got);
			hex(tag, pk, 1 + midi_cin_length(pk[0]));
		}
	printf("KAT midi packets: %d\n", got);
	return 0;
}
