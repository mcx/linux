# SPDX-License-Identifier: GPL-2.0-only
#
# linux/arch/arm64/crypto/Makefile
#
# Copyright (C) 2014 Linaro Ltd <ard.biesheuvel@linaro.org>
#

obj-$(CONFIG_CRYPTO_SHA3_ARM64) += sha3-ce.o
sha3-ce-y := sha3-ce-glue.o sha3-ce-core.o

obj-$(CONFIG_CRYPTO_SM3_NEON) += sm3-neon.o
sm3-neon-y := sm3-neon-glue.o sm3-neon-core.o

obj-$(CONFIG_CRYPTO_SM3_ARM64_CE) += sm3-ce.o
sm3-ce-y := sm3-ce-glue.o sm3-ce-core.o

obj-$(CONFIG_CRYPTO_SM4_ARM64_CE) += sm4-ce-cipher.o
sm4-ce-cipher-y := sm4-ce-cipher-glue.o sm4-ce-cipher-core.o

obj-$(CONFIG_CRYPTO_SM4_ARM64_CE_BLK) += sm4-ce.o
sm4-ce-y := sm4-ce-glue.o sm4-ce-core.o

obj-$(CONFIG_CRYPTO_SM4_ARM64_CE_CCM) += sm4-ce-ccm.o
sm4-ce-ccm-y := sm4-ce-ccm-glue.o sm4-ce-ccm-core.o

obj-$(CONFIG_CRYPTO_SM4_ARM64_CE_GCM) += sm4-ce-gcm.o
sm4-ce-gcm-y := sm4-ce-gcm-glue.o sm4-ce-gcm-core.o

obj-$(CONFIG_CRYPTO_SM4_ARM64_NEON_BLK) += sm4-neon.o
sm4-neon-y := sm4-neon-glue.o sm4-neon-core.o

obj-$(CONFIG_CRYPTO_GHASH_ARM64_CE) += ghash-ce.o
ghash-ce-y := ghash-ce-glue.o ghash-ce-core.o

obj-$(CONFIG_CRYPTO_POLYVAL_ARM64_CE) += polyval-ce.o
polyval-ce-y := polyval-ce-glue.o polyval-ce-core.o

obj-$(CONFIG_CRYPTO_AES_ARM64_CE) += aes-ce-cipher.o
aes-ce-cipher-y := aes-ce-core.o aes-ce-glue.o

obj-$(CONFIG_CRYPTO_AES_ARM64_CE_CCM) += aes-ce-ccm.o
aes-ce-ccm-y := aes-ce-ccm-glue.o aes-ce-ccm-core.o

obj-$(CONFIG_CRYPTO_AES_ARM64_CE_BLK) += aes-ce-blk.o
aes-ce-blk-y := aes-glue-ce.o aes-ce.o

obj-$(CONFIG_CRYPTO_AES_ARM64_NEON_BLK) += aes-neon-blk.o
aes-neon-blk-y := aes-glue-neon.o aes-neon.o

obj-$(CONFIG_CRYPTO_NHPOLY1305_NEON) += nhpoly1305-neon.o
nhpoly1305-neon-y := nh-neon-core.o nhpoly1305-neon-glue.o

obj-$(CONFIG_CRYPTO_AES_ARM64) += aes-arm64.o
aes-arm64-y := aes-cipher-core.o aes-cipher-glue.o

obj-$(CONFIG_CRYPTO_AES_ARM64_BS) += aes-neon-bs.o
aes-neon-bs-y := aes-neonbs-core.o aes-neonbs-glue.o
