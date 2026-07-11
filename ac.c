#include "ac.h"

#include <assert.h>
#include <stdlib.h>

void count_cum_freqs(struct symbol *table, size_t symbols)
{
	if (symbols > 0) {
		table[0].cum_freq = 0;

		for (size_t i = 1; i < symbols; i++) {
			table[i].cum_freq =
				table[i-1].cum_freq +
				table[i-1].freq;
		}

	}
}

size_t calc_total_freq(struct symbol *table, size_t symbols)
{
	size_t total = 0;

	for (size_t i = 0; i < symbols; i++) {
		total += table[i].freq;
	}

	return total;
}

const size_t g_FirstQuarter = 0x20000000;
const size_t g_ThirdQuarter = 0x60000000;
const size_t g_Half         = 0x40000000;

void ac_init(struct ac *ac)
{
	ac->mLow  = 0x00000000;
	ac->mHigh = 0x7FFFFFFF;

	ac->mScale = 0;
}

#define put_bit(bio, b) bio_write_bits((bio), (b), 1)
#define get_bit(bio) bio_read_bits((bio), 1)

void ac_encode_scale(struct ac *ac, struct bio *bio)
{
	for (;;) {
		if (ac->mHigh < g_Half) {
			put_bit(bio, 0);

			while (ac->mScale > 0) {
				put_bit(bio, 1);
				ac->mScale--;
			}
		}
		else if (ac->mLow >= g_Half) {
			put_bit(bio, 1);

			while (ac->mScale > 0) {
				put_bit(bio, 0);
				ac->mScale--;
			}

			ac->mLow  -= g_Half;
			ac->mHigh -= g_Half;
		}
		else if (ac->mLow >= g_FirstQuarter &&
				 ac->mHigh < g_ThirdQuarter) {
			ac->mScale++;
			ac->mLow  -= g_FirstQuarter;
			ac->mHigh -= g_FirstQuarter;
				 }
		else {
			break;
		}

		ac->mLow  <<= 1;
		ac->mHigh = (ac->mHigh << 1) | 1;
	}
}

void ac_encode(struct ac *ac, struct bio *bio, size_t low_freq, size_t high_freq, size_t total)
{
	uint64_t range =
		(uint64_t)ac->mHigh - ac->mLow + 1;

	uint64_t new_low =
		range * low_freq / total;

	uint64_t new_high =
		range * high_freq / total;

	ac->mLow += (uint32_t)new_low;
	ac->mHigh = ac->mLow + (uint32_t)(new_high - new_low) - 1;

	ac_encode_scale(ac, bio);
}

size_t index_of_symbol(size_t symb, struct symbol *model, size_t symbols)
{
	if (symb < symbols && model[symb].symb == symb) {
		return symb;
	}

	for (size_t i = 0; i < symbols; i++) {
		if (symb == model[i].symb) {
			return i;
		}
	}

	abort();
}

void ac_encode_symbol(struct ac *ac, struct bio *bio, size_t symb, struct symbol *model, size_t symbols, size_t total_count)
{
	size_t index = index_of_symbol(symb, model, symbols);

	size_t low_freq  = model[index].cum_freq;
	size_t high_freq = model[index].cum_freq + model[index].freq;

	ac_encode(ac, bio, low_freq, high_freq, total_count);
}

float ac_encode_symbol_query_prob(size_t symb, struct symbol *model, size_t symbols, size_t total_count)
{
	size_t index = index_of_symbol(symb, model, symbols);

	return (float)model[index].freq / total_count;
}

void ac_encode_bypass_bits(
	struct bio *bio,
	uint32_t value,
	size_t bit_count)
{
	bio_write_bits(bio, value, bit_count);
}

uint32_t ac_decode_bypass_bits(
	struct bio *bio,
	size_t bit_count)
{
	return bio_read_bits(bio, bit_count);
}

void ac_encode_flush(struct ac *ac, struct bio *bio)
{
	ac->mScale++;

	if (ac->mLow < g_FirstQuarter) {
		put_bit(bio, 0);

		while (ac->mScale-- > 0) {
			put_bit(bio, 1);
		}
	} else {
		put_bit(bio, 1);

		while (ac->mScale-- > 0) {
			put_bit(bio, 0);
		}
	}
}

static size_t ac_decode_target(const struct ac *ac, size_t total)
{
	const uint64_t range =
		(uint64_t)ac->mHigh - ac->mLow + 1;

	const uint64_t offset =
		(uint64_t)ac->mBuffer - ac->mLow + 1;

	return (offset * total - 1) / range;
}

void ac_decode_init(struct ac *ac, struct bio *bio)
{
	ac->mBuffer = 0;

	for (size_t i = 0; i < 31; i++) {
		ac->mBuffer = (ac->mBuffer << 1) | get_bit(bio);
	}
}

void ac_decode_scale(struct ac *ac, struct bio *bio)
{
	for (;;) {
		if (ac->mHigh < g_Half) {
			// E1: no subtraction is needed.
		}
		else if (ac->mLow >= g_Half) {
			// E2
			ac->mLow    -= g_Half;
			ac->mHigh   -= g_Half;
			ac->mBuffer -= g_Half;
		}
		else if (ac->mLow >= g_FirstQuarter &&
				 ac->mHigh < g_ThirdQuarter) {
			// E3
			ac->mLow    -= g_FirstQuarter;
			ac->mHigh   -= g_FirstQuarter;
			ac->mBuffer -= g_FirstQuarter;
				 }
		else {
			break;
		}

		ac->mLow    <<= 1;
		ac->mHigh    = (ac->mHigh << 1) | 1u;
		ac->mBuffer  = (ac->mBuffer << 1) | get_bit(bio);
	}
}

size_t index_of_value(size_t value, struct symbol *model, size_t symbols)
{
	size_t lo = 0;
	size_t hi = symbols;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		size_t high_freq = model[mid].cum_freq + model[mid].freq;

		if (value < high_freq) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}

	if (lo < symbols) {
		size_t low_freq  = model[lo].cum_freq;
		size_t high_freq = low_freq + model[lo].freq;

		if (value >= low_freq && value < high_freq) {
			return lo;
		}
	}

	abort();
}

size_t ac_decode_symbol(struct ac *ac, struct bio *bio, struct symbol *model, size_t symbols, size_t total)
{
	const size_t value = ac_decode_target(ac, total);

	const size_t index =
		index_of_value(value, model, symbols);

	const size_t low_freq  = model[index].cum_freq;
	const size_t high_freq = low_freq + model[index].freq;

	const uint64_t range =
		(uint64_t)ac->mHigh - ac->mLow + 1;

	const uint64_t new_low =
		range * low_freq / total;

	const uint64_t new_high =
		range * high_freq / total;

	ac->mLow += (uint32_t)new_low;
	ac->mHigh =
		ac->mLow + (uint32_t)(new_high - new_low) - 1;

	ac_decode_scale(ac, bio);

	return model[index].symb;
}

void ac_encode_symbol_model(struct ac *ac, struct bio *bio, size_t symb, struct model *model)
{
	ac_encode_symbol(ac, bio, symb, model->table, model->count, model->total);
}

float ac_encode_symbol_model_query_prob(size_t symb, struct model *model)
{
	return ac_encode_symbol_query_prob(symb, model->table, model->count, model->total);
}

size_t ac_decode_symbol_model(struct ac *ac, struct bio *bio, struct model *model)
{
	const size_t value = ac_decode_target(ac, model->total);
	size_t index;

	if (model->decode_lut != NULL &&
		model->decode_lut_size == model->total) {
		index = model->decode_lut[value];
	} else {
		index = index_of_value(value, model->table, model->count);
	}

	const size_t low_freq  = model->table[index].cum_freq;
	const size_t high_freq = low_freq + model->table[index].freq;

	const uint64_t range =
		(uint64_t)ac->mHigh - ac->mLow + 1;

	const uint64_t new_low =
		range * low_freq / model->total;

	const uint64_t new_high =
		range * high_freq / model->total;

	ac->mLow += (uint32_t)new_low;
	ac->mHigh =
		ac->mLow + (uint32_t)(new_high - new_low) - 1;

	ac_decode_scale(ac, bio);

	return model->table[index].symb;
}

void inc_model(struct model *model, size_t symbol)
{
	size_t index = index_of_symbol(symbol, model->table, model->count);

	model_clear_decode_lut(model);
	model->table[index].freq++;
	model->total++;

	count_cum_freqs(model->table, model->count);
}

void model_create(struct model *model, size_t size)
{
	assert(model != NULL);

	model->count = size;
	model->total = 0;
	model->table = malloc(model->count * sizeof(struct symbol));
	model->decode_lut_size = 0;
	model->decode_lut = NULL;

	if (model->table == NULL) {
		abort();
	}

	for (size_t i = 0; i < model->count; ++i) {
		model->table[i].symb = i;
		model->table[i].freq = 1;
	}

	count_cum_freqs(model->table, model->count);
	model->total = calc_total_freq(model->table, model->count);
}

void model_enlarge(struct model *model)
{
	assert(model != NULL);

	model_clear_decode_lut(model);
	model->count++;
	model->table = realloc(model->table, model->count * sizeof(struct symbol));

	if (model->table == NULL) {
		abort();
	}

	model->table[model->count - 1].symb = model->count - 1;
	model->table[model->count - 1].freq = 1;

	count_cum_freqs(model->table, model->count);
	model->total = calc_total_freq(model->table, model->count);
}

void model_destroy(struct model *model)
{
	assert(model != NULL);

	model_clear_decode_lut(model);
	free(model->table);
}

void model_clear_decode_lut(struct model *model)
{
	if (model == NULL) {
		return;
	}

	free(model->decode_lut);
	model->decode_lut = NULL;
	model->decode_lut_size = 0;
}

int model_build_decode_lut(struct model *model, size_t max_total)
{
	if (model == NULL || model->table == NULL ||
		model->total == 0 || model->total > max_total ||
		model->count > UINT16_MAX) {
		model_clear_decode_lut(model);
		return model != NULL && model->total == 0;
	}

	model_clear_decode_lut(model);

	model->decode_lut = malloc(model->total * sizeof(model->decode_lut[0]));
	if (model->decode_lut == NULL) {
		return 0;
	}

	model->decode_lut_size = model->total;

	for (size_t i = 0; i < model->count; ++i) {
		const size_t low = model->table[i].cum_freq;
		const size_t high = low + model->table[i].freq;

		if (model->table[i].freq == 0) {
			continue;
		}

		for (size_t value = low; value < high; ++value) {
			model->decode_lut[value] = (uint16_t)i;
		}
	}

	return 1;
}

enum {
	MODEL_MAX_TOTAL = 1u << 20
};

void model_rescale(struct model *model)
{
	model_clear_decode_lut(model);

	for (size_t i = 0; i < model->count; ++i) {
		model->table[i].freq =
			(model->table[i].freq + 1) / 2;

		if (model->table[i].freq == 0) {
			model->table[i].freq = 1;
		}
	}

	count_cum_freqs(model->table, model->count);
	model->total = calc_total_freq(model->table, model->count);
}

void model_update(struct model *model, size_t symbol)
{
	size_t index =
		index_of_symbol(symbol, model->table, model->count);

	model->table[index].freq++;
	model->total++;

	model_clear_decode_lut(model);

	if (model->total >= MODEL_MAX_TOTAL) {
		model_rescale(model);
	} else {
		count_cum_freqs(model->table, model->count);
	}
}
