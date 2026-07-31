#include <stdint.h>
#include <stdio.h>

#include "../memory_layout.h"

int main(void)
{
	uint32_t p96_end =
		ZZ_Z3_P96_MEMORY_ARM_START + ZZ_Z3_P96_MEMORY_SIZE;

	if (ZZ_Z3_P96_MEMORY_SIZE != 0x02e00000UL) {
		fprintf(stderr, "unexpected Z3 P96 memory size: 0x%08lx\n",
		        (unsigned long)ZZ_Z3_P96_MEMORY_SIZE);
		return 1;
	}
	if (p96_end != ZZ_SDK_SHARED_HEAP_ARM_START) {
		fprintf(stderr,
		        "P96/SDK boundary mismatch: 0x%08lx != 0x%08lx\n",
		        (unsigned long)p96_end,
		        (unsigned long)ZZ_SDK_SHARED_HEAP_ARM_START);
		return 1;
	}

	puts("memory layout tests passed");
	return 0;
}
