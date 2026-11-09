/* SPDX-License-Identifier: MPL-2.0 */
#ifndef RX3_BEATJUMP_CUSTOM_CONFIG_H
#define RX3_BEATJUMP_CUSTOM_CONFIG_H

/* RX3_RUNTIME/beatjump/beatjump.ini, written by the app:
 *
 *   version=1
 *   pad1=-1          BEAT JUMP, pads 1..8
 *   ...
 *   pad16=+64        BEAT JUMP 2, pads 9..16 (pad 9 is its pad 1)
 *   led_color=#RRGGBB  optional
 *
 * A value is a signed number of beats: a mandatory sign, then 0.5 or a whole
 * number from 1 to 64 with no leading zero (64 beats = 16 bars in 4/4).
 * A pad that is not named keeps the player's own jump. Blank lines and lines
 * starting with '#' are skipped; CRLF is accepted. Anything else (unknown key,
 * duplicate key, spaces, a value out of range, a missing version) rejects the
 * whole file, and the deck stays stock: no half-applied configuration. */
#define BEATJUMP_PAD_COUNT 16u
#define BEATJUMP_CONFIG_MAX_BYTES 4096u
#define BEATJUMP_MAX_HALF_BEATS 128 /* 64 beats: the user's limit */
#define BEATJUMP_LED_DEFAULT 0xffffffu

struct beatjump_config {
    /* Half beats, signed: negative jumps back. 0 = the pad stays stock. */
    int half[BEATJUMP_PAD_COUNT];
    uint32_t rgb;
    /* 1 only when the file names led_color: otherwise the pads keep the
       player's own lights, the same colour as BEAT JUMP (user's choice). */
    unsigned int custom_led;
};

static void beatjump_config_defaults(struct beatjump_config *config)
{
    for (unsigned int pad = 0; pad < BEATJUMP_PAD_COUNT; pad++)
        config->half[pad] = 0;
    config->rgb = BEATJUMP_LED_DEFAULT;
    config->custom_led = 0u;
}

/* "+0.5", "-1" ... "+256" -> half beats, 0 when malformed. */
static int beatjump_parse_value(const char *value, size_t length)
{
    if (length < 2u || (value[0] != '+' && value[0] != '-'))
        return 0;
    int sign = value[0] == '-' ? -1 : 1;
    value++;
    length--;
    if (length == 3u && value[0] == '0' && value[1] == '.' && value[2] == '5')
        return sign;
    if (length > 3u || value[0] < '1' || value[0] > '9')
        return 0;
    int beats = 0;
    for (size_t i = 0; i < length; i++) {
        if (value[i] < '0' || value[i] > '9')
            return 0;
        beats = beats * 10 + (value[i] - '0');
    }
    if (beats * 2 > BEATJUMP_MAX_HALF_BEATS)
        return 0;
    return sign * beats * 2;
}

static int beatjump_parse_colour(const char *value, size_t length, uint32_t *rgb)
{
    if (length != 7u || value[0] != '#')
        return 0;
    uint32_t colour = 0u;
    for (unsigned int i = 1u; i < 7u; i++) {
        unsigned int digit;
        if (value[i] >= '0' && value[i] <= '9')
            digit = (unsigned int)(value[i] - '0');
        else if (value[i] >= 'a' && value[i] <= 'f')
            digit = (unsigned int)(value[i] - 'a') + 10u;
        else if (value[i] >= 'A' && value[i] <= 'F')
            digit = (unsigned int)(value[i] - 'A') + 10u;
        else
            return 0;
        colour = (colour << 4u) | digit;
    }
    *rgb = colour;
    return 1;
}

/* "pad1".."pad16" -> 0..15, else -1. */
static int beatjump_pad_key(const char *key, size_t length)
{
    if (length < 4u || length > 5u || memcmp(key, "pad", 3u))
        return -1;
    if (key[3] < '1' || key[3] > '9')
        return -1;
    int number = key[3] - '0';
    if (length == 5u) {
        if (key[4] < '0' || key[4] > '9')
            return -1;
        number = number * 10 + (key[4] - '0');
    }
    return number >= 1 && number <= (int)BEATJUMP_PAD_COUNT ? number - 1 : -1;
}

/* Returns 1 and fills `config` for a valid file; 0 leaves the defaults
   (every pad stock) in `config`. */
static int beatjump_parse_config(const char *text, size_t length,
                                 struct beatjump_config *config)
{
    struct beatjump_config next;
    beatjump_config_defaults(config);
    beatjump_config_defaults(&next);
    if (!text || !length || length > BEATJUMP_CONFIG_MAX_BYTES)
        return 0;
    /* Bit 0..15 the pads, 16 version, 17 led_color: each key at most once. */
    uint32_t seen = 0u;
    size_t cursor = 0u;
    while (cursor < length) {
        size_t start = cursor;
        while (cursor < length && text[cursor] != '\n')
            cursor++;
        size_t end = cursor;
        if (cursor < length)
            cursor++;
        if (end > start && text[end - 1u] == '\r')
            end--;
        if (end == start || text[start] == '#')
            continue;
        size_t equals = start;
        while (equals < end && text[equals] != '=')
            equals++;
        if (equals == end)
            return 0;
        const char *key = text + start;
        size_t key_length = equals - start;
        const char *value = text + equals + 1u;
        size_t value_length = end - equals - 1u;
        int pad = beatjump_pad_key(key, key_length);
        if (pad >= 0) {
            uint32_t bit = 1u << (unsigned int)pad;
            int half = beatjump_parse_value(value, value_length);
            if ((seen & bit) || !half)
                return 0;
            next.half[pad] = half;
            seen |= bit;
        } else if (key_length == 7u && !memcmp(key, "version", 7u)) {
            if ((seen & 0x10000u) || value_length != 1u || value[0] != '1')
                return 0;
            seen |= 0x10000u;
        } else if (key_length == 9u && !memcmp(key, "led_color", 9u)) {
            if ((seen & 0x20000u) || !beatjump_parse_colour(value, value_length, &next.rgb))
                return 0;
            seen |= 0x20000u;
            next.custom_led = 1u;
        } else {
            return 0;
        }
    }
    if (!(seen & 0x10000u))
        return 0;
    *config = next;
    return 1;
}

#endif /* RX3_BEATJUMP_CUSTOM_CONFIG_H */
