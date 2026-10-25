// Portable BPE encoder: text -> token ids.
//
// It must reproduce `tokenizers` exactly -- a prompt tokenized differently from
// training is silently out of distribution, with no error to notice.
// firmware/host_verify/tok_test.c diffs this against the Python tokenizer for
// that reason, driven by src/tok_check.py.
//
// Two stages, matching prepare.py's pre_tokenizer Sequence
// (Digits{individual_digits:true} + ByteLevel{add_prefix_space:false}):
//   1. Pre-tokenize. Merges never cross a chunk boundary, so getting this wrong
//      changes the output even when the merge table is perfect.
//   2. Within each chunk, seed one token per byte and repeatedly apply the
//      lowest-rank adjacent merge. Real BPE, not greedy longest-match: greedy
//      matching agrees with BPE often enough to look correct in spot checks and
//      then disagrees on exactly the words that matter.
//
// **Digits are one chunk each and never take the leading space.** The Digits
// pre-tokenizer runs before ByteLevel, so " 100" is four pre-tokens -- a lone
// space then three digits -- not one. This is not cosmetic here: v1 emits pin
// numbers digit by digit, so a device that grouped "100" or glued the space on
// would feed the model a token sequence it never saw in training, for exactly
// the inputs where a wrong answer moves a physical pin.
//
// Unicode limitation, now UTF-8 aware: classification decodes code points and
// covers the ranges a command corpus contains (CJK, kana, Hangul, fullwidth
// forms). An unlisted script falls into punctuation, which is the same class
// the byte-based version effectively gave it -- correct for nothing it is asked
// to do here, and checked against Python on every corpus line by
// src/tok_check.py rather than assumed.
#ifndef TOKENIZER_H
#define TOKENIZER_H
#include <stdint.h>
#include <string.h>

// Merge table view. Entries are sorted by `keys` so lookup is a binary search;
// `rank` is BPE merge priority (lower wins), `newtok` the resulting token.
typedef struct {
  const uint16_t *byte_tok;  // [256] byte -> seed token id
  const uint32_t *keys;      // [n] sorted (id_a << 16) | id_b
  const uint16_t *rank;      // [n]
  const uint16_t *newtok;    // [n]
  int n;
  // Decode direction. Only the name span needs it, but it is part of the same
  // generated pair as the tables above and must never be swapped independently.
  const uint16_t *tok_off;   // [vocab + 1] offsets into tok_bytes
  const uint8_t *tok_bytes;  // raw bytes of every token, concatenated
  int vocab;
} Bpe;

#define TOK_NO_RANK 0xFFFF

// Character classes, UTF-8 aware.
//
// **The ASCII-only version was wrong for Chinese, and silently so.** GPT-2's
// pattern classifies with Unicode properties; the first version here classified
// bytes, so every byte >= 0x80 fell into the "other" class. For an ASCII corpus
// that never showed: training and device agreed because there was nothing above
// 0x7F. With a Chinese corpus it breaks everywhere CJK punctuation appears --
// Python splits "打开，灯" into letters("打开") + punct("，") + letters("灯")
// (three chunks, merges cannot cross a boundary) while a byte classifier makes
// one run of "other" bytes and merges straight through the comma. The prompt
// then reaches the model tokenized differently from anything it was trained on:
// no error, no log line, just worse parses. Verified by tok_check.py over real
// corpus lines, which is the only reason to trust it.
//
// The classes are the ones a command corpus can contain, not all of Unicode:
// CJK ideographs, kana and Hangul as letters; CJK/fullwidth forms split into
// their letter, digit and punctuation parts; everything else punctuation.
enum { TOK_CLS_SPACE = 0, TOK_CLS_LETTER, TOK_CLS_DIGIT, TOK_CLS_PUNCT };

static inline int tok_utf8_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;                       // invalid lead byte: one opaque byte
}

// Decode the code point at i. Sets *len to the bytes consumed (1 on any
// malformed sequence, which keeps the walker moving instead of derailing).
static uint32_t tok_cp(const char *s, int n, int i, int *len) {
  unsigned char c = (unsigned char)s[i];
  int l = tok_utf8_len(c);
  if (l <= 1 || i + l > n) { *len = 1; return c; }
  uint32_t cp = (l == 2) ? (c & 0x1Fu) : (l == 3) ? (c & 0x0Fu) : (c & 0x07u);
  for (int k = 1; k < l; k++) {
    unsigned char cc = (unsigned char)s[i + k];
    if ((cc & 0xC0) != 0x80) { *len = 1; return c; }
    cp = (cp << 6) | (cc & 0x3Fu);
  }
  *len = l;
  return cp;
}

static int tok_class(const char *s, int n, int i, int *len) {
  uint32_t cp = tok_cp(s, n, i, len);
  if (cp < 0x80) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
        cp == '\f' || cp == '\v') return TOK_CLS_SPACE;
    if (cp >= '0' && cp <= '9') return TOK_CLS_DIGIT;
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))
      return TOK_CLS_LETTER;
    return TOK_CLS_PUNCT;
  }
  if (cp == 0x00A0 || cp == 0x3000) return TOK_CLS_SPACE;
  if (cp >= 0x3400 && cp <= 0x4DBF) return TOK_CLS_LETTER;   // CJK ext A
  if (cp >= 0x4E00 && cp <= 0x9FFF) return TOK_CLS_LETTER;   // CJK unified
  if (cp >= 0xF900 && cp <= 0xFAFF) return TOK_CLS_LETTER;   // compat ideographs
  if (cp >= 0x3040 && cp <= 0x30FF) return TOK_CLS_LETTER;   // kana
  if (cp >= 0xAC00 && cp <= 0xD7AF) return TOK_CLS_LETTER;   // hangul
  if (cp >= 0xFF10 && cp <= 0xFF19) return TOK_CLS_DIGIT;    // fullwidth digits
  if ((cp >= 0xFF21 && cp <= 0xFF3A) ||
      (cp >= 0xFF41 && cp <= 0xFF5A)) return TOK_CLS_LETTER;
  return TOK_CLS_PUNCT;
}

static inline int tok_is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Rank of the pair (a, b), or TOK_NO_RANK if it is not a merge. Sets *out to
// the merged token when found.
static int tok_pair_rank(const Bpe *bpe, uint16_t a, uint16_t b, uint16_t *out) {
  uint32_t key = ((uint32_t)a << 16) | (uint32_t)b;
  int lo = 0, hi = bpe->n - 1;
  while (lo <= hi) {
    int mid = lo + ((hi - lo) >> 1);
    uint32_t k = bpe->keys[mid];
    if (k == key) { *out = bpe->newtok[mid]; return bpe->rank[mid]; }
    if (k < key) lo = mid + 1; else hi = mid - 1;
  }
  return TOK_NO_RANK;
}

// GPT-2 contractions ('s 't 're 've 'm 'll 'd), matched before everything else.
// Returns the chunk length, or 0 if none applies.
static int tok_contraction(const char *s, int n, int i) {
  if (s[i] != '\'' || i + 1 >= n) return 0;
  char a = s[i + 1];
  char b = (i + 2 < n) ? s[i + 2] : 0;
  if (a == 's' || a == 't' || a == 'm' || a == 'd') return 2;
  if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) return 3;
  return 0;
}

// Length of the pre-token starting at i, following GPT-2's alternation order:
//   's|'t|'re|'ve|'m|'ll|'d | ' ?\p{L}+' | ' ?\p{N}+' | ' ?[^\s\p{L}\p{N}]+'
//   | '\s+(?!\S)' | '\s+'
// with the Digits pre-tokenizer overriding the number branch (see the header).
// The optional leading space is a literal ASCII space, not any whitespace,
// which is why a chunk can absorb " word" but never "\nword".
//
// Runs are walked by code point (tok_class) rather than by byte, so a CJK
// character counts as one letter and CJK punctuation as one punctuation mark --
// the same split the Python pre-tokenizer makes.
static int tok_chunk_len(const char *s, int n, int i) {
  int c = tok_contraction(s, n, i);
  if (c) return c;

  int len = 1;
  int here = tok_class(s, n, i, &len);

  // A digit is always alone, and a space before one stays behind as its own
  // chunk: " 100" -> " ", "1", "0", "0". Checked before the leading-space skip
  // below, which would otherwise hand the space to the digit.
  if (here == TOK_CLS_DIGIT) return len;
  if (here == TOK_CLS_SPACE && s[i] == ' ' && i + 1 < n) {
    int l2 = 1;
    if (tok_class(s, n, i + 1, &l2) == TOK_CLS_DIGIT) return 1;
  }

  int j = i + (s[i] == ' ' ? 1 : 0);

  if (j < n) {
    int lj = 1;
    int cj = tok_class(s, n, j, &lj);
    if (cj == TOK_CLS_LETTER || cj == TOK_CLS_PUNCT) {
      int k = j;
      while (k < n) {
        int l = 1;
        if (tok_class(s, n, k, &l) != cj) break;
        k += l;
      }
      if (k > j) return k - i;
    }
  }

  // Whitespace run. '\s+(?!\S)' means a run followed by a non-space keeps its
  // last space for the following chunk's optional leading space.
  int k = i, last = 1;
  while (k < n) {
    int l = 1;
    if (tok_class(s, n, k, &l) != TOK_CLS_SPACE) break;
    last = l;
    k += l;
  }
  if (k < n && k - i > 1) k -= last;
  return k > i ? k - i : 1;
}

// Encode one pre-token chunk, appending ids to out. Returns tokens written.
static int tok_encode_chunk(const Bpe *bpe, const char *s, int len,
                            int *out, int max_out) {
  if (len <= 0 || max_out <= 0) return 0;

  // Seed one token per byte, capped by the caller's buffer.
  int count = len < max_out ? len : max_out;
  for (int i = 0; i < count; i++)
    out[i] = bpe->byte_tok[(unsigned char)s[i]];

  // Repeatedly apply the single lowest-rank adjacent merge.
  while (count > 1) {
    int best_rank = TOK_NO_RANK, best_at = -1;
    uint16_t best_new = 0;
    for (int p = 0; p + 1 < count; p++) {
      uint16_t merged;
      int r = tok_pair_rank(bpe, (uint16_t)out[p], (uint16_t)out[p + 1], &merged);
      if (r < best_rank) { best_rank = r; best_at = p; best_new = merged; }
    }
    if (best_at < 0) break;
    out[best_at] = best_new;
    memmove(&out[best_at + 1], &out[best_at + 2],
            (size_t)(count - best_at - 2) * sizeof(int));
    count--;
  }
  return count;
}

// Encode NUL-terminated text. Returns the number of ids written (<= max_out).
static int tok_encode(const Bpe *bpe, const char *text, int *out, int max_out) {
  int n = (int)strlen(text), i = 0, total = 0;
  while (i < n && total < max_out) {
    int len = tok_chunk_len(text, n, i);
    total += tok_encode_chunk(bpe, text + i, len, out + total, max_out - total);
    i += len;
  }
  return total;
}

// Decode `n` ids into `out` as a NUL-terminated string. Returns the length
// written, or -1 if an id is out of range or the text does not fit.
//
// Used for one thing: turning a copied name span back into the string the alias
// table is keyed on. It is a concatenation, not an inverse BPE -- gen_assets.py
// already un-mapped ByteLevel's codepoints back to bytes, so there is nothing
// to undo here.
//
// -1 rather than a truncated string on overflow, deliberately. A name cut short
// is still a *valid* name, and would resolve to a different device than the one
// asked for -- the same substitution this design exists to prevent, arriving
// through a buffer instead of through the model.
static int tok_decode(const Bpe *bpe, const int *ids, int n,
                      char *out, int max_out) {
  int len = 0;
  for (int i = 0; i < n; i++) {
    int id = ids[i];
    if (id < 0 || id >= bpe->vocab) return -1;
    int lo = bpe->tok_off[id], hi = bpe->tok_off[id + 1];
    if (len + (hi - lo) >= max_out) return -1;      // >= leaves room for NUL
    for (int k = lo; k < hi; k++) out[len++] = (char)bpe->tok_bytes[k];
  }
  out[len] = 0;
  return len;
}

#endif
