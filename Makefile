AMIGA_IMAGE ?= sacredbanana/amiga-compiler:m68k-amigaos
AMIGA_DOCKER = ./tools/amiga-docker.sh

.PHONY: all build-all package-local check-release quality rtg-tests \
	rtg zztop zzscanlines zzfwupdate usb-poseidon sd-boot net ZZNetStats \
	mhi ahi axmp3 ZZDiag sdk amissl

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

rtg:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) rtg ./build.sh

zztop:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ZZTop ./build-gcc.sh

zzscanlines:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ZZScanlines \
		m68k-amigaos-gcc -O2 -noixemul -I../include \
		-o ZZScanlines ZZScanlines.c -lamiga

zzfwupdate:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ZZFwUpdate \
		m68k-amigaos-gcc -O2 -noixemul -Wall -Wextra -I../include \
		-o ZZFwUpdate ZZFwUpdate.c -lamiga

usb-poseidon:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) usb-poseidon ./build.sh

sd-boot:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) sd-boot ./build.sh

net:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) net make

ZZNetStats:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) net/ZZNetStats \
		m68k-amigaos-gcc -O2 -noixemul -Wall -Wextra \
		-Wno-unused-parameter -I../../include \
		-o ZZNetStats ZZNetStats.c -lamiga

mhi:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) mhi ./build.sh

ahi:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ahi/driver ./build.sh

axmp3:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ax-direct ./build-gcc.sh

ZZDiag:
	AMIGA_IMAGE="$(AMIGA_IMAGE)" $(AMIGA_DOCKER) ZZDiag ./build.sh

# Host-side: these drive their own container invocations.
sdk:
	sdk/build.sh

amissl:
	amissl/build.sh
