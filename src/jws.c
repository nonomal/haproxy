/* SPDX-License-Identifier: GPL-2.0-or-later */


#include <haproxy/base64.h>
#include <haproxy/chunk.h>
#include <haproxy/init.h>
#include <haproxy/openssl-compat.h>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#if defined(HAVE_JWS)

/*
 * Convert an OpenSSL BIGNUM to a base64url representation
 * Fill a buffer <dst> of <dsize> max size
 *
 * Return the size of the data dumped in <dst>
 */
int bn2base64url(const BIGNUM *bn, char *dst, size_t dsize)
{
	struct buffer *bin;
	int binlen;
	int ret = 0;

	if ((bin = get_trash_chunk()) == NULL)
		goto out;

	binlen = BN_num_bytes(bn);
	if (binlen > bin->size)
		goto out;

	if (BN_bn2bin(bn, (unsigned char *)bin->area) != binlen)
		goto out;

	ret = a2base64url(bin->area, binlen, dst, dsize);
out:
	return ret;
}

/*
 * Convert a EC <pkey> to a public key JWK
 * Fill a buffer <dst> of <dsize> max size
 *
 * Return the size of the data or 0
 */
static int EVP_PKEY_EC_to_pub_jwk(EVP_PKEY *pkey, char *dst, size_t dsize)
{
	BIGNUM *x = NULL, *y = NULL;
	struct buffer *str_x = NULL, *str_y = NULL;
	int ret = 0;

#if HA_OPENSSL_VERSION_NUMBER > 0x30000000L
	char crv[32];
	size_t crvlen;

	EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_X, &x);
	EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_EC_PUB_Y, &y);

	if (EVP_PKEY_get_utf8_string_param(pkey, OSSL_PKEY_PARAM_GROUP_NAME, crv, sizeof(crv), &crvlen) == 0)
		goto out;
#else
	const char *crv = NULL;
	const EC_KEY *ec = NULL;
	const EC_GROUP *ec_group = NULL;
	const EC_POINT *ec_point = NULL;

	/* get EC from EVP */
	if ((ec = EVP_PKEY_get0_EC_KEY(pkey)) == NULL)
		goto out;
	if ((ec_group = EC_KEY_get0_group(ec)) == NULL)
		goto out;
	if ((ec_point = EC_KEY_get0_public_key(ec)) == NULL)
		goto out;

	/* get group, point, x, y */
	if ((x = BN_new()) == NULL)
		goto out;
	if ((y = BN_new()) == NULL)
		goto out;
	if ((EC_POINT_get_affine_coordinates(ec_group, ec_point, x, y, NULL)) == 0)
		goto out;
	if ((crv = EC_curve_nid2nist(EC_GROUP_get_curve_name(ec_group))) == NULL)
		goto out;
#endif

	/* allocate trash */
	if ((str_x = alloc_trash_chunk()) == NULL)
		goto out;
	if ((str_y = alloc_trash_chunk()) == NULL)
		goto out;

	/* convert x, y to base64url */
	str_x->data = bn2base64url(x, str_x->area, str_x->size);
	str_y->data = bn2base64url(y, str_y->area, str_y->size);
	if (str_x->data == 0 || str_y->data == 0)
		goto out;

	ret = snprintf(dst, dsize, "{\n"
			"    \"kty\": \"%s\",\n"
			"    \"crv\": \"%s\",\n"
			"    \"x\":   \"%s\",\n"
			"    \"y\":   \"%s\"\n"
			"}\n",
			"EC", crv, str_x->area, str_y->area);
	if (ret >= dsize)
		ret = 0;

out:
	free_trash_chunk(str_x);
	free_trash_chunk(str_y);

#if HA_OPENSSL_VERSION_NUMBER < 0x30000000L
	BN_free(x);
	BN_free(y);
#endif

	return ret;
}

/*
 * Convert a RSA <pkey> to a public key JWK
 * Fill a buffer <dst> of <dsize> max size
 *
 * Return the size of the data or 0
 */
static int EVP_PKEY_RSA_to_pub_jwk(EVP_PKEY *pkey, char *dst, size_t dsize)
{
	BIGNUM *n = NULL, *e = NULL;
	struct buffer *str_n = NULL, *str_e = NULL;
	int ret = 0;

#if HA_OPENSSL_VERSION_NUMBER > 0x30000000L

	if ((EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n)) == 0)
		goto out;
	if ((EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e)) == 0)
		goto out;
#else
	const RSA *rsa;

	if ((rsa = EVP_PKEY_get0_RSA(pkey)) == NULL)
		goto out;
	if ((n = (BIGNUM *)RSA_get0_n(rsa)) == NULL)
		goto out;
	if ((e = (BIGNUM *)RSA_get0_e(rsa)) == NULL)
		goto out;
#endif

	/* allocate trash */
	if ((str_n = alloc_trash_chunk()) == NULL)
		goto out;
	if ((str_e = alloc_trash_chunk()) == NULL)
		goto out;

	/* convert n, e to base64url */
	str_n->data = bn2base64url(n, str_n->area, str_n->size);
	str_e->data = bn2base64url(e, str_e->area, str_e->size);
	if (str_n->data == 0 || str_e->data == 0)
		goto out;

	ret = snprintf(dst, dsize, "{\n"
			"    \"kty\": \"%s\",\n"
			"    \"n\":   \"%s\",\n"
			"    \"e\":   \"%s\"\n"
			"}\n",
			"RSA", str_n->area, str_e->area);
	if (ret >= dsize)
		ret = 0;

out:
	free_trash_chunk(str_n);
	free_trash_chunk(str_e);

	return ret;
}

/* Convert an EVP_PKEY to a public key JWK
 * Fill a buffer <dst> of <dsize> max size
 *
 * Return the size of the data or 0
 */
int EVP_PKEY_to_pub_jwk(EVP_PKEY *pkey, char *dst, size_t dsize)
{
	int ret = 0;

	switch (EVP_PKEY_base_id(pkey)) {
		case EVP_PKEY_RSA:
			ret = EVP_PKEY_RSA_to_pub_jwk(pkey, dst, dsize);
			break;
		case EVP_PKEY_EC:
			ret = EVP_PKEY_EC_to_pub_jwk(pkey, dst, dsize);
			break;
		default:
			break;
	}
	return ret;
}

int jwk_debug(int argc, char **argv)
{
	FILE *f = NULL;
	EVP_PKEY *pkey = NULL;
	char msg[1024];
	int ret = 1;
	const char *filename;

	if (argc < 1)
		goto out;

	filename = argv[1];

	if ((f = fopen(filename, "r")) == NULL) {
		fprintf(stderr, "fopen!\n");
		goto out;
	}
	if ((pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL)) == NULL) {
		fprintf(stderr, "PEM_read_PrivateKey!\n");
		goto out;
	}

	ret = !!EVP_PKEY_to_pub_jwk(pkey, msg, sizeof(msg));
	fprintf(stderr, "%s\n", msg);

out:

	return ret;
}

static void __jws_init(void)
{
	hap_register_unittest("jwk", jwk_debug);
}


INITCALL0(STG_REGISTER, __jws_init);

#endif /* HAVE_JWS */

