/*
 * libFuzzer harness for the MIDI byte-stream -> USB-MIDI packet parser
 * (Firmware/midi/midi.h), which the TRS jacks and the radio link both feed.
 *
 * byte 0 bit 0 turns SysEx on (the nRF54 path) or off (the TRS path).  The
 * rest is the byte stream.  Every packet that comes out is checked against
 * the CIN tables: a channel or system-common packet carries only data bytes
 * after its status byte, and a SysEx packet has a CIN of 4 to 7.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Firmware/midi/midi.h"

static void need(bool ok)
{
	if (!ok)
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct midi_parser p;
	uint8_t packet[4];

	if (size < 1 || size > 4096)
		return -1;

	memset(&p, 0, sizeof p);
	p.want_sysex = data[0] & 1;

	for (size_t i = 1; i < size; i++) {
		bool sysex_before = p.in_sysex;

		if (!midi_parse_byte(&p, data[i], packet))
			continue;

		need(packet[0] <= 0x0f);
		if (sysex_before) {
			need(p.want_sysex);
			need(packet[0] >= 0x04 && packet[0] <= 0x07);
			need(packet[0] != 0x04 || p.in_sysex);
			need(packet[0] == 0x04 || !p.in_sysex);
		} else {
			int n = midi_cin_length(packet[0]);

			need(!p.in_sysex);
			need(packet[0] == midi_status_cin(packet[1]));
			need(packet[1] >= 0x80 && packet[1] < 0xf8);
			need(n >= 1 && n <= 3);
			for (int k = 1; k < n; k++)
				need(packet[1 + k] < 0x80);
		}
	}
	return 0;
}
