// SPDX-License-Identifier: GPL-2.0-only
/*
 * GHASH using the RISC-V vector crypto extensions
 *
 * Copyright (C) 2023 VRULL GmbH
 * Author: Heiko Stuebner <heiko.stuebner@vrull.eu>
 *
 * Copyright (C) 2023 SiFive, Inc.
 * Author: Jerry Shih <jerry.shih@sifive.com>
 */

#include <asm/simd.h>
#include <asm/vector.h>
#include <crypto/b128ops.h>
#include <crypto/gf128mul.h>
#include <crypto/ghash.h>
#include <crypto/internal/hash.h>
#include <crypto/internal/simd.h>
#include <crypto/utils.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

asmlinkage void ghash_zvkg(be128 *accumulator, const be128 *key, const u8 *data,
			   size_t len);

struct riscv64_ghash_tfm_ctx {
	be128 key;
};

struct riscv64_ghash_desc_ctx {
	be128 accumulator;
};

static int riscv64_ghash_setkey(struct crypto_shash *tfm, const u8 *key,
				unsigned int keylen)
{
	struct riscv64_ghash_tfm_ctx *tctx = crypto_shash_ctx(tfm);

	if (keylen != GHASH_BLOCK_SIZE)
		return -EINVAL;

	memcpy(&tctx->key, key, GHASH_BLOCK_SIZE);

	return 0;
}

static int riscv64_ghash_init(struct shash_desc *desc)
{
	struct riscv64_ghash_desc_ctx *dctx = shash_desc_ctx(desc);

	*dctx = (struct riscv64_ghash_desc_ctx){};

	return 0;
}

static inline void
riscv64_ghash_blocks(const struct riscv64_ghash_tfm_ctx *tctx,
		     struct riscv64_ghash_desc_ctx *dctx,
		     const u8 *src, size_t srclen)
{
	/* The srclen is nonzero and a multiple of 16. */
	if (crypto_simd_usable()) {
		kernel_vector_begin();
		ghash_zvkg(&dctx->accumulator, &tctx->key, src, srclen);
		kernel_vector_end();
	} else {
		do {
			crypto_xor((u8 *)&dctx->accumulator, src,
				   GHASH_BLOCK_SIZE);
			gf128mul_lle(&dctx->accumulator, &tctx->key);
			src += GHASH_BLOCK_SIZE;
			srclen -= GHASH_BLOCK_SIZE;
		} while (srclen);
	}
}

static int riscv64_ghash_update(struct shash_desc *desc, const u8 *src,
				unsigned int srclen)
{
	const struct riscv64_ghash_tfm_ctx *tctx = crypto_shash_ctx(desc->tfm);
	struct riscv64_ghash_desc_ctx *dctx = shash_desc_ctx(desc);

	riscv64_ghash_blocks(tctx, dctx, src,
			     round_down(srclen, GHASH_BLOCK_SIZE));
	return srclen - round_down(srclen, GHASH_BLOCK_SIZE);
}

static int riscv64_ghash_finup(struct shash_desc *desc, const u8 *src,
			       unsigned int len, u8 *out)
{
	const struct riscv64_ghash_tfm_ctx *tctx = crypto_shash_ctx(desc->tfm);
	struct riscv64_ghash_desc_ctx *dctx = shash_desc_ctx(desc);

	if (len) {
		u8 buf[GHASH_BLOCK_SIZE] = {};

		memcpy(buf, src, len);
		riscv64_ghash_blocks(tctx, dctx, buf, GHASH_BLOCK_SIZE);
		memzero_explicit(buf, sizeof(buf));
	}

	memcpy(out, &dctx->accumulator, GHASH_DIGEST_SIZE);
	return 0;
}

static struct shash_alg riscv64_ghash_alg = {
	.init = riscv64_ghash_init,
	.update = riscv64_ghash_update,
	.finup = riscv64_ghash_finup,
	.setkey = riscv64_ghash_setkey,
	.descsize = sizeof(struct riscv64_ghash_desc_ctx),
	.digestsize = GHASH_DIGEST_SIZE,
	.base = {
		.cra_blocksize = GHASH_BLOCK_SIZE,
		.cra_ctxsize = sizeof(struct riscv64_ghash_tfm_ctx),
		.cra_priority = 300,
		.cra_flags = CRYPTO_AHASH_ALG_BLOCK_ONLY,
		.cra_name = "ghash",
		.cra_driver_name = "ghash-riscv64-zvkg",
		.cra_module = THIS_MODULE,
	},
};

static int __init riscv64_ghash_mod_init(void)
{
	if (riscv_isa_extension_available(NULL, ZVKG) &&
	    riscv_vector_vlen() >= 128)
		return crypto_register_shash(&riscv64_ghash_alg);

	return -ENODEV;
}

static void __exit riscv64_ghash_mod_exit(void)
{
	crypto_unregister_shash(&riscv64_ghash_alg);
}

module_init(riscv64_ghash_mod_init);
module_exit(riscv64_ghash_mod_exit);

MODULE_DESCRIPTION("GHASH (RISC-V accelerated)");
MODULE_AUTHOR("Heiko Stuebner <heiko.stuebner@vrull.eu>");
MODULE_LICENSE("GPL");
MODULE_ALIAS_CRYPTO("ghash");
