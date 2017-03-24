#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "util.h"

int main() {
	uint8_t *occupancy = calloc(1, 1<<29);
	uint32_t state = 1;
	uint32_t count = 0;
	while (true) {
		if (state == 0) {
			fprintf(stderr, "zero touched at %i\n", count);
			return 0;
		}
		int byte = state / 8, bit = state % 8;
		int mask = 1 << bit;
		if (occupancy[byte] & mask) {
			fprintf(stderr, "collision at round %i, value %i\n", count, state);
			return 0;
		}
		occupancy[byte] |= mask;
		state = lcg_parkmiller(state);
		count ++;
		if (count % 1048576 == 0) fprintf(stderr, ": round %08x\n", count);
	}
}
