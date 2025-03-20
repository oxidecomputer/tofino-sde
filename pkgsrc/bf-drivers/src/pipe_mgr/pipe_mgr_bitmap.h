/*******************************************************************************
 *  Copyright (C) 2024 Intel Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions
 *  and limitations under the License.
 *
 *
 *  SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/


#ifndef PIPE_MGR_BITMAP_H_INCLUDED
#define PIPE_MGR_BITMAP_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>
#ifdef __sun
#include <endian.h>
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#else
#include <sys/param.h>  // for MIN()
#endif // __sun
#include <pipe_mgr/pipe_mgr_config.h>

/** atomic bitmap storage type. */
typedef uint32_t pipe_bitmap_word_t;
#define PIPE_BMP_WORD_SIZE 1
/** All bitmaps must contain this structure */
typedef struct pipe_bitmap_hdr_s {
  /** The number of words in this bitmap */
  int wordcount;
  /** bitmap words */
  pipe_bitmap_word_t words[PIPE_BMP_WORD_SIZE];
  /** Maximum allowable bit */
  uint32_t maxbit;
} pipe_bitmap_hdr_t;

/** The number of bits in each bitmap_word */
#define PIPE_BITMAP_BITS_PER_WORD \
  (sizeof(pipe_bitmap_word_t) * 8) /* use CHAR_BITS here */

/** The number of words required to store a given bitcount */
#define PIPE_BITMAP_WORD_COUNT(_bitcount)      \
  (((_bitcount) / PIPE_BITMAP_BITS_PER_WORD) + \
   (((_bitcount) % PIPE_BITMAP_BITS_PER_WORD) ? 1 : 0))

/** A dynamically allocated bitmap. */
typedef struct pipe_bitmap_ {
  /** bitmap hdr */
  pipe_bitmap_hdr_t hdr;
} pipe_bitmap_t;

/** Get a word from the give bitmap header */
#define PIPE_BITMAP_HDR_WORD_GET(_hdr, _word) ((_hdr)->words[_word])

/** Get the word containing the given bit */
#define PIPE_BITMAP_HDR_BIT_WORD(_hdr, _bit) \
  ((_hdr)->words[_bit / PIPE_BITMAP_BITS_PER_WORD])

/** Get the bit's position in its target word */
#define PIPE_BITMAP_BIT_POS(_bit) ((1L << (_bit % PIPE_BITMAP_BITS_PER_WORD)))

/** Check if two bitmaps are of same size */
#define PIPE_BITMAP_SIZE_EQ(_hdr_a, _hdr_b)        \
  (((_hdr_a)->wordcount == (_hdr_b)->wordcount) && \
   ((_hdr_a)->maxbit == (_hdr_b)->maxbit))

/** return the size of a bitmap */
#define PIPE_BITMAP_BITCOUNT(_bmp) ((_bmp)->hdr.maxbit + 1)

static inline void pipe_bitmap_init(pipe_bitmap_hdr_t *hdr, int bitcount) {
  int i = 0;

  hdr->wordcount = PIPE_BITMAP_WORD_COUNT(bitcount);
  PIPE_MGR_ASSERT(hdr->wordcount <= PIPE_BMP_WORD_SIZE);

  for (i = 0; i < PIPE_BMP_WORD_SIZE; i++) {
    hdr->words[i] = 0;
  }
  hdr->maxbit = bitcount - 1;
}

/**
 * @brief Set a bit in the bitmap.
 * @param hdr The bitmap header.
 * @param bit The bit number to set.
 */
static inline void pipe_bitmap_set(pipe_bitmap_hdr_t *hdr, int bit) {
  PIPE_BITMAP_HDR_BIT_WORD(hdr, bit) |= PIPE_BITMAP_BIT_POS(bit);
}

/**
 * @brief Clear a bit in the bitmap.
 * @param hdr The bitmap header.
 * @param bit The bit number to clear.
 */
static inline void pipe_bitmap_clr(pipe_bitmap_hdr_t *hdr, int bit) {
  PIPE_BITMAP_HDR_BIT_WORD(hdr, bit) &= ~(PIPE_BITMAP_BIT_POS(bit));
}

/**
 * @brief Clear a bitset in the bitmap.
 * @param hdr The bitmap header.
 * @param clrbit_hdr The bits to clear.
 */
static inline void pipe_bitmap_clr_bmap(pipe_bitmap_hdr_t *hdr,
                                        pipe_bitmap_hdr_t *clr_hdr) {
  int idx;
  if (!PIPE_BITMAP_SIZE_EQ(hdr, clr_hdr)) {
    PIPE_MGR_ASSERT(0);
  }
  for (idx = 0; idx < hdr->wordcount; idx++) {
    hdr->words[idx] &= ~clr_hdr->words[idx];
  }
}

/**
 * @brief Modify a bit in the bitmap.
 * @param hdr The bitmap header.
 * @param bit The bit number to modify.
 * @param value The bit's new value.
 */
static inline void pipe_bitmap_mod(pipe_bitmap_hdr_t *hdr, int bit, int value) {
  if (value) {
    pipe_bitmap_set(hdr, bit);
  } else {
    pipe_bitmap_clr(hdr, bit);
  }
}
/**
 * @brief Get the value of a bit in the bitmap.
 * @param hdr The bitmap header.
 * @param bit The bit number.
 */
static inline int pipe_bitmap_get(pipe_bitmap_hdr_t *hdr, int bit) {
  if (bit < 0 || (unsigned int)bit > hdr->maxbit) return 0;
  return (PIPE_BITMAP_HDR_BIT_WORD(hdr, bit) & PIPE_BITMAP_BIT_POS(bit)) ? 1
                                                                         : 0;
}

/**
 * @brief Set all bits in the given bitmap.
 * @param hdr The bitmap header.
 */
static inline void pipe_bitmap_set_all(pipe_bitmap_hdr_t *hdr) {
  // PIPE_MGR_MEMSET(hdr->words, 0xFF,
  // hdr->wordcount*sizeof(pipe_bitmap_word_t));
  uint32_t bit = 0;
  for (bit = 0; bit <= hdr->maxbit; bit++) {
    pipe_bitmap_set(hdr, bit);
  }
}
/**
 * @brief Clear all bits in the given bitmap.
 * @param hdr The bitmap header.
 */
static inline void pipe_bitmap_clr_all(pipe_bitmap_hdr_t *hdr) {
  PIPE_MGR_MEMSET(hdr->words, 0x0, hdr->wordcount * sizeof(pipe_bitmap_word_t));
}

/**
 * @brief Get number of bits that are set in the bitmap.
 * @param hdr The bitmap header.
 */
static inline int pipe_bitmap_count(pipe_bitmap_hdr_t *hdr) {
  int idx = 0, bit_count = 0;
  pipe_bitmap_word_t word;

  for (; idx < hdr->wordcount; idx++) {
    word = hdr->words[idx];
    while (word) {
      word = word & (word - 1);
      bit_count++;
    }
  }

  return bit_count;
}

/**
 * @brief Check if both bitmaps are equal.
 * @param hdr_a The bitmap header.
 * @param hdr_b The bitmap header.
 */
static inline int pipe_bitmap_is_eq(pipe_bitmap_hdr_t *hdr_a,
                                    pipe_bitmap_hdr_t *hdr_b) {
  if (!PIPE_BITMAP_SIZE_EQ(hdr_a, hdr_b)) {
    PIPE_MGR_ASSERT(0);
  }

  if (PIPE_MGR_MEMCMP(hdr_a->words,
                      hdr_b->words,
                      hdr_a->wordcount * sizeof(pipe_bitmap_word_t)) == 0) {
    return 1;
  }

  return 0;
}

/**
 * @brief Assign second bitmap to first one.
 * @param hdr_a The bitmap header.
 * @param hdr_b The bitmap header.
 */
static inline void pipe_bitmap_assign(pipe_bitmap_hdr_t *hdr_a,
                                      pipe_bitmap_hdr_t *hdr_b) {
  if (!PIPE_BITMAP_SIZE_EQ(hdr_a, hdr_b)) {
    PIPE_MGR_ASSERT(0);
  }

  PIPE_MGR_MEMCPY(hdr_a->words,
                  hdr_b->words,
                  hdr_a->wordcount * sizeof(pipe_bitmap_word_t));
}

/**
 * @brief Performs binary OR operation on bitmaps.
 * @param hdr_a The bitmap header.
 * @param hdr_b The bitmap header.
 */
static inline void pipe_bitmap_or(pipe_bitmap_hdr_t *hdr_a,
                                  pipe_bitmap_hdr_t *hdr_b) {
  int idx = 0;

  if (!PIPE_BITMAP_SIZE_EQ(hdr_a, hdr_b)) {
    PIPE_MGR_ASSERT(0);
  }

  for (; idx < hdr_a->wordcount; idx++) {
    hdr_a->words[idx] |= hdr_b->words[idx];
  }
}

/**
 * @brief Performs binary AND operation on bitmaps.
 * @param hdr_a The bitmap header.
 * @param hdr_b The bitmap header.
 */
static inline void pipe_bitmap_and(pipe_bitmap_hdr_t *hdr_a,
                                   pipe_bitmap_hdr_t *hdr_b) {
  int idx = 0;

  if (!PIPE_BITMAP_SIZE_EQ(hdr_a, hdr_b)) {
    PIPE_MGR_ASSERT(0);
  }

  for (; idx < hdr_a->wordcount; idx++) {
    hdr_a->words[idx] &= hdr_b->words[idx];
  }
}

/**
 * @brief Performs binary AND operation on bitmaps. Result is not stored
 * @param hdr_a The bitmap header.
 * @param hdr_b The bitmap header.
 */
static inline bool pipe_bitmap_chk(pipe_bitmap_hdr_t *hdr_a,
                                   pipe_bitmap_hdr_t *hdr_b) {
  int idx;
  int count = MIN(hdr_a->wordcount, hdr_b->wordcount);
  for (idx = 0; idx < count; idx++) {
    if (hdr_a->words[idx] & hdr_b->words[idx]) {
      return true;
    }
  }
  return false;
}

static inline bool pipe_bitmap_is_all_set(pipe_bitmap_t *bmap) {
  uint32_t bit = 0;
  PIPE_MGR_ASSERT((bmap)->hdr.maxbit != 0);
  for (bit = 0; bit <= (bmap)->hdr.maxbit; bit++) {
    if (!pipe_bitmap_get(&(bmap->hdr), bit)) {
      return false;
    }
  }
  return true;
}

static inline int pipe_bitmap_get_next_bit(pipe_bitmap_t *bmap, int curr_bit) {
  uint32_t start_bit = 0;

  if (curr_bit == -1) {
    start_bit = 0;
  } else {
    start_bit = curr_bit + 1;
  }

  for (; start_bit <= (bmap)->hdr.maxbit; start_bit++) {
    if (pipe_bitmap_get(&(bmap->hdr), start_bit)) {
      return start_bit;
    }
  }
  return -1;
}

static inline uint32_t pipe_bitmap_get_first_free(pipe_bitmap_t *bmap) {
  uint32_t start_bit = 0;

  for (start_bit = 0; start_bit <= (bmap)->hdr.maxbit; start_bit++) {
    if (!pipe_bitmap_get(&(bmap->hdr), start_bit)) {
      return start_bit;
    }
  }
  return -1;
}

static inline uint32_t pipe_bitmap_get_first_set(pipe_bitmap_t *bmap) {
  int start_word = 0;

  for (start_word = 0; start_word < bmap->hdr.wordcount; start_word++) {
    if (bmap->hdr.words[start_word] == 0) {
      continue;
    }
    return __builtin_ctz(bmap->hdr.words[start_word]);
  }

  return -1;
}

#define PIPE_BITMAP_INIT(_bmap, _bit) pipe_bitmap_init(&((_bmap)->hdr), _bit);

/** Set a bit */
#define PIPE_BITMAP_SET(_bmap, _bit) pipe_bitmap_set(&((_bmap)->hdr), _bit);

/** Clear a bit */
#define PIPE_BITMAP_CLR(_bmap, _bit) pipe_bitmap_clr(&((_bmap)->hdr), _bit)

/** Clear a set of bits */
#define PIPE_BITMAP_CLR_BITS(_bmap, _clr_bmap) \
  pipe_bitmap_clr_bmap(&((_bmap)->hdr), &((_clr_bmap)->hdr))

/** Modify a bit */
#define PIPE_BITMAP_MOD(_bmap, _bit, _value) \
  pipe_bitmap_mod(&((_bmap)->hdr), _bit, _value)

/** Get a bit */
#define PIPE_BITMAP_GET(_bmap, _bit) pipe_bitmap_get(&((_bmap)->hdr), _bit)

/** Get number of bits set */
#define PIPE_BITMAP_COUNT(_bmap) pipe_bitmap_count(&((_bmap)->hdr))

/** Set all bits */
#define PIPE_BITMAP_SET_ALL(_bmap) pipe_bitmap_set_all(&((_bmap)->hdr))

/** Clear all bits */
#define PIPE_BITMAP_CLR_ALL(_bmap) pipe_bitmap_clr_all(&((_bmap)->hdr))

/** Iterate through all bits set in the bitmap */
#define PIPE_BITMAP_ITER(_bmap, _bit)                 \
  for (_bit = 0; _bit <= (_bmap)->hdr.maxbit; _bit++) \
    if (PIPE_BITMAP_GET(_bmap, _bit))

/** See if both bitmaps are equal */
#define PIPE_BITMAP_IS_EQ(_bmap_a, _bmap_b) \
  pipe_bitmap_is_eq(&((_bmap_a)->hdr), &((_bmap_b)->hdr))

/** See if both bitmaps are not equal */
#define PIPE_BITMAP_IS_NEQ(_bmap_a, _bmap_b) \
  (!PIPE_BITMAP_IS_EQ(_bmap_a, _bmap_b))

/** Assigns _bmap_b to _bmap_a */
#define PIPE_BITMAP_ASSIGN(_bmap_a, _bmap_b) \
  pipe_bitmap_assign(&((_bmap_a)->hdr), &((_bmap_b)->hdr))

/** bitmap_a |= _bmap_b */
#define PIPE_BITMAP_OR(_bmap_a, _bmap_b) \
  pipe_bitmap_or(&((_bmap_a)->hdr), &((_bmap_b)->hdr))

/** bitmap_a &= _bmap_b */
#define PIPE_BITMAP_AND(_bmap_a, _bmap_b) \
  pipe_bitmap_and(&((_bmap_a)->hdr), &((_bmap_b)->hdr))

/** Get first free bit */
#define PIPE_BITMAP_GET_FIRST_FREE(_bmap) pipe_bitmap_get_first_free(_bmap)

/** bitmap_a & _bmap_b */
#define PIPE_BITMAP_CHK(_bmap_a, _bmap_b) \
  pipe_bitmap_chk(&((_bmap_a)->hdr), &((_bmap_b)->hdr))

/* get next set bit */
#define PIPE_BITMAP_GET_NEXT_BIT(_bmap, _bit) \
  pipe_bitmap_get_next_bit(_bmap, _bit)

#define PIPE_BITMAP_IS_ALL_SET(_bmap) pipe_bitmap_is_all_set(_bmap)

#define PIPE_BITMAP_GET_FIRST_SET(_bmap) pipe_bitmap_get_first_set(_bmap)

#endif  // PIPE_MGR_BITMAP_H_INCLUDED
