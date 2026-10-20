#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../cardmem_protocol.h"

static int failures;
static int checks;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

static void test_request_marshalling(void)
{
	uint32_t words[2] = { 0, 0 };
	const uint8_t *bytes = (const uint8_t *)words;

	CHECK(zz_cardmem_round_size(0) == 0);
	CHECK(zz_cardmem_round_size(1) == ZZ_CARDMEM_GRANULARITY);
	CHECK(zz_cardmem_round_size(ZZ_CARDMEM_GRANULARITY) ==
		ZZ_CARDMEM_GRANULARITY);
	CHECK(zz_cardmem_round_size(ZZ_CARDMEM_GRANULARITY + 1) ==
		ZZ_CARDMEM_GRANULARITY * 2);
	CHECK(zz_cardmem_round_size(UINT32_MAX) == 0);
	CHECK(zz_cardmem_alignment(320, 1) == ZZ_CARDMEM_GRANULARITY);
	CHECK(!zz_cardmem_status_accepted(0));
	CHECK(zz_cardmem_status_accepted(1));

	zz_cardmem_marshal_alloc(words, 0x12300, ZZ_CARDMEM_GRANULARITY);
	CHECK(bytes[0] == 0x00 && bytes[1] == 0x00 &&
		bytes[2] == 0x01 && bytes[3] == 0x00);
	CHECK(bytes[4] == 0x00 && bytes[5] == 0x01 &&
		bytes[6] == 0x23 && bytes[7] == 0x00);
	zz_cardmem_store_be32(&words[0], 0x12300);
	CHECK(zz_cardmem_unmarshal_reply(words) == 0x12300);
	zz_cardmem_store_be32(&words[0], 0);
	CHECK(zz_cardmem_unmarshal_reply(words) == 0);
	CHECK(zz_cardmem_status_accepted(1));

	memset(words, 0, sizeof(words));
	zz_cardmem_marshal_abs(words, 0x02000000, 513);
	CHECK(zz_cardmem_load_be32(&words[0]) == 0x02000000);
	CHECK(zz_cardmem_load_be32(&words[1]) == 513);
}


int main(void)
{
	test_request_marshalling();

	if (failures) {
		printf("%d of %d card-memory checks failed\n", failures, checks);
		return 1;
	}
	printf("all %d card-memory checks passed\n", checks);
	return 0;
}
