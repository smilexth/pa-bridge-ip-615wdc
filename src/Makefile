# Build inside a container matching the RUNTIME's Debian release. Building on a
# newer Debian than the target produces a binary that will not start there
# (glibc is backward-compatible, not forward-compatible) — see
# docs/build-and-deploy.md. The repo root has a Dockerfile that does this.

SDK_DIR ?= ../vendor/sdk
SDK_LIB  = $(SDK_DIR)/Include/libCtsSdk.a
CFLAGS  += -O2 -Wall -Wextra -std=gnu11 -I$(SDK_DIR)/Include
# libCtsSdk.a is not built as PIE, so the final link must not be either.
LDFLAGS += -no-pie
LIBS     = $(SDK_LIB) -lpthread -lm

pa-bridge: check-sdk pa-bridge.c
	$(CC) $(CFLAGS) pa-bridge.c $(LIBS) $(LDFLAGS) -o $@

# The SDK is BUENA's, not ours to redistribute, so it is not in this repo.
# Fail with an explanation rather than a linker error nobody can act on.
check-sdk:
	@test -f $(SDK_LIB) || { \
	  echo ""; \
	  echo "  The vendor SDK is missing: $(SDK_LIB)"; \
	  echo ""; \
	  echo "  It is BUENA's proprietary library and is not distributed here."; \
	  echo "  Download 'IP-Series System SDK Package' from the vendor's site,"; \
	  echo "  then copy the x86-64 build's Include/ directory into place:"; \
	  echo ""; \
	  echo "      unzip ip-series-sdk.zip"; \
	  echo "      tar xJf 'SDK/SDK Linux/SDK__Linux__X64__V308__New.tar.xz'"; \
	  echo "      mkdir -p $(SDK_DIR)"; \
	  echo "      cp -r SDK__Linux__X64__V308__New/Include $(SDK_DIR)/"; \
	  echo ""; \
	  echo "  Take the X64 V308 build. The newer V5086 tree is ARM despite"; \
	  echo "  'X64' appearing in one of its directory names, and linking it"; \
	  echo "  on x86-64 fails with 'relocations in generic ELF (EM: 40)'."; \
	  echo ""; \
	  exit 1; \
	}

clean:
	rm -f pa-bridge

.PHONY: clean check-sdk
