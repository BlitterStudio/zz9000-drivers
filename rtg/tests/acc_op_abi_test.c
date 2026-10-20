#include <stdio.h>

/* zz9000.h uses the Amiga compiler's short aliases directly. */
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned long uint32;

#include "../zz9000.h"

int main(void)
{
	if (ACC_OP_CARDMEM_ALLOC != 13 ||
		ACC_OP_CARDMEM_ALLOC_ABS != 14 ||
		ACC_OP_CARDMEM_FREE != 15) {
		fprintf(stderr, "card-memory accelerator ABI mismatch\n");
		return 1;
	}
	puts("card-memory accelerator ABI: 13/14/15");
	return 0;
}
