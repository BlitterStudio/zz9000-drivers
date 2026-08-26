#ifndef ZZ_MHI_TRANSPORT_GEOMETRY_H
#define ZZ_MHI_TRANSPORT_GEOMETRY_H

#include <stdint.h>

struct zz_mhi_transport_geometry {
	uint32_t staging_bytes;
	uint32_t mp3_ring_bytes;
	uint32_t pcm_ring_bytes;
	uint32_t pcm_low_water_bytes;
};

/* Z2 keeps the qualified compact footprint. Z3 batches at the same geometry as
 * ZZPlay's passing accelerated-AHI path, avoiding 4-KiB mailbox/feed storms. */
static inline struct zz_mhi_transport_geometry
zz_mhi_transport_geometry(uint32_t zorro_version)
{
	struct zz_mhi_transport_geometry geometry;

	if (zorro_version == 3U) {
		geometry.staging_bytes = 64U * 1024U;
		geometry.mp3_ring_bytes = 128U * 1024U;
		geometry.pcm_ring_bytes = 256U * 1024U;
	} else {
		geometry.staging_bytes = 4U * 1024U;
		geometry.mp3_ring_bytes = 8U * 1024U;
		geometry.pcm_ring_bytes = 61440U;
	}
	geometry.pcm_low_water_bytes = geometry.pcm_ring_bytes / 2U;
	return geometry;
}

#endif
