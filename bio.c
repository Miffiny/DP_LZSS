#include "bio.h"

#include <assert.h>

static int bio_can_access_word(const struct bio *bio)
{
	const char *ptr;
	const char *end;

	if (bio == NULL || bio->ptr == NULL || bio->end == NULL) {
		return 0;
	}

	ptr = (const char *)bio->ptr;
	end = (const char *)bio->end;

	return ptr + sizeof(uint32_t) <= end;
}

static uint32_t low_mask(size_t n)
{
	if (n == 0) {
		return 0;
	}

	if (n >= 32) {
		return UINT32_MAX;
	}

	return ((uint32_t)1 << n) - 1;
}

void bio_open(struct bio *bio, void *ptr, void *end, int mode)
{
	assert(bio != NULL);

	bio->ptr = ptr;
	bio->end = end;
	bio->b = 0;

	if (mode == BIO_MODE_READ) {
		bio->c = 32;
	} else {
		bio->b = 0;
		bio->c = 0;
	}
}

static void bio_flush_buffer(struct bio *bio)
{
	assert(bio != NULL);
	assert(bio->ptr != NULL);
	assert(bio_can_access_word(bio));

	*(bio->ptr++) = bio->b;
	bio->b = 0;
	bio->c = 0;
}

static void bio_reload_buffer(struct bio *bio)
{
	assert(bio != NULL);
	assert(bio->ptr != NULL);

	if (bio_can_access_word(bio)) {
		bio->b = *(bio->ptr++);
	} else {
		bio->b = 0;
	}

	bio->c = 0;
}

static size_t minsize(size_t a, size_t b)
{
	return a < b ? a : b;
}

void bio_write_bits(struct bio *bio, uint32_t b, size_t n)
{
	assert(bio != NULL);
	assert(n <= 32);

	while (n > 0) {
		assert(bio->c < 32);

		size_t m = minsize(32 - bio->c, n);

		bio->b |= (b & low_mask(m)) << bio->c;
		bio->c += m;

		if (bio->c == 32) {
			bio_flush_buffer(bio);
		}

		if (m == 32) {
			b = 0;
		} else {
			b >>= m;
		}
		n -= m;
	}
}

uint32_t bio_read_bits(struct bio *bio, size_t n)
{
	assert(bio != NULL);
	assert(n <= 32);

	if (bio->c == 32) {
		bio_reload_buffer(bio);
	}

	/* get the avail. least-significant bits */
	size_t s = minsize(32 - bio->c, n);

	uint32_t w = bio->b & low_mask(s);

	if (s == 32) {
		bio->b = 0;
	} else {
		bio->b >>= s;
	}
	bio->c += s;

	n -= s;

	/* need more bits? reload & get the most-significant bits */
	if (n > 0) {
		assert(bio->c == 32);

		bio_reload_buffer(bio);

		w |= (bio->b & low_mask(n)) << s;

		bio->b >>= n;
		bio->c += n;
	}

	return w;
}

void bio_close(struct bio *bio, int mode)
{
	assert(bio != NULL);

	if (mode == BIO_MODE_WRITE && bio->c > 0) {
		bio_flush_buffer(bio);
	}
}
