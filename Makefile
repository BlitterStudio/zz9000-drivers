AMIGA_IMAGE ?= amigadev/crosstools:m68k-amigaos-gcc10
AMIGA_DOCKER = ./tools/amiga-docker.sh
# LRA (-mlra) exists on Bebbo GCC 10+ only; the older 6.5.0b rejects it.
LRA_SH = lra=; m68k-amigaos-gcc -mlra -x c -fsyntax-only /dev/null 2>/dev/null && lra=-mlra; exec

.PHONY: all build-all package-local check-release quality rtg-tests mhi-tests \
	rtg zztop zzscanlines zzfwupdate usb-poseidon sd-boot net ZZNetStats \
	mhi ahi ahi-duplextest ZZDiag sdk amissl

all: build-all

build-all:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" ./tools/build-all.sh

package-local:
	./tools/package-local.sh

check-release:
	./tools/check-release.sh

quality:
	./tools/check-quality.sh

rtg-tests:
	$(MAKE) -C rtg/tests test

mhi-tests:
	$(CC) -std=c99 -O2 -Wall -Wextra -Werror \
		-o mhi/transport_geometry_test mhi/transport_geometry_test.c
	./mhi/transport_geometry_test
	rm -f mhi/transport_geometry_test

rtg:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) rtg ./build.sh

zztop:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ZZTop ./build-gcc.sh

zzscanlines:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ZZScanlines sh -c \
		'$(LRA_SH) m68k-amigaos-gcc -O2 $$lra -noixemul -I../include \
		-o ZZScanlines ZZScanlines.c -lamiga'

zzfwupdate:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ZZFwUpdate sh -c \
		'$(LRA_SH) m68k-amigaos-gcc -O2 $$lra -noixemul -Wall -Wextra -I../include \
		-I../common -o ZZFwUpdate ZZFwUpdate.c ../common/fwup_amiga.c \
		../common/fwup_client.c -lamiga'

usb-poseidon:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) usb-poseidon ./build.sh

sd-boot:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) sd-boot ./build.sh

net:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) net make

ZZNetStats:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) net/ZZNetStats sh -c \
		'$(LRA_SH) m68k-amigaos-gcc -O2 $$lra -noixemul -Wall -Wextra \
		-Wno-unused-parameter -I../../include \
		-o ZZNetStats ZZNetStats.c -lamiga'

# mhi/build.sh stages zz9k headers from the sibling zz9000-sdk checkout on
# the host, then re-execs itself through amiga-docker.sh.
mhi:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" mhi/build.sh

ahi:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ahi/driver ./build.sh

ahi-duplextest:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ahi/duplextest ./build.sh

ZZDiag:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ZZDiag ./build.sh

# Host-side: these drive their own container invocations.
sdk:
	sdk/build.sh

amissl:
	amissl/build.sh
