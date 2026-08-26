#include <assert.h>

#include "transport_geometry.h"

int main(void)
{
	struct zz_mhi_transport_geometry z2 = zz_mhi_transport_geometry(2U);
	struct zz_mhi_transport_geometry z3 = zz_mhi_transport_geometry(3U);

	assert(z2.staging_bytes == 4U * 1024U);
	assert(z2.mp3_ring_bytes == 8U * 1024U);
	assert(z2.pcm_ring_bytes == 61440U);
	assert(z2.pcm_low_water_bytes == 30720U);

	assert(z3.staging_bytes == 64U * 1024U);
	assert(z3.mp3_ring_bytes == 128U * 1024U);
	assert(z3.pcm_ring_bytes == 256U * 1024U);
	assert(z3.pcm_low_water_bytes == 128U * 1024U);
	assert(z3.mp3_ring_bytes >= z3.staging_bytes * 2U);
	return 0;
}
