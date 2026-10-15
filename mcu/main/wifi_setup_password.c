/* SPDX-License-Identifier: MIT */
#include "wifi_setup_password.h"
#include <stddef.h>
#include <string.h>

/* Four-letter words keep the password short and fit the handheld display.
 * Uppercase matches the Apple II font used for the on-device setup details.
 */
static const char words[][5] = {
    "ACRE", "ARCH", "ATOM", "AUNT", "BABY", "BAKE", "BALL", "BAND", "BANK", "BARK", "BARN", "BASE", "BATH", "BEAD", "BEAK", "BEAM",
    "BEAN", "BEAR", "BEAT", "BEEF", "BELL", "BELT", "BEND", "BEST", "BIKE", "BIRD", "BITE", "BLUE", "BOAT", "BODY", "BOLT", "BONE",
    "BOOK", "BOOT", "BOWL", "BRAN", "BREW", "BRIM", "BROW", "BULB", "BUMP", "BUSH", "BUSY", "CAKE", "CALM", "CAMP", "CANE", "CAPE",
    "CARD", "CARE", "CARP", "CART", "CASE", "CAVE", "CHAT", "CHIN", "CHIP", "CITY", "CLAM", "CLAY", "CLIP", "CLUB", "COAL", "COAT",
    "CODE", "COIL", "COIN", "COLD", "COLT", "COMB", "COOK", "COOL", "CORD", "CORE", "CORK", "CORN", "COZY", "CRAB", "CREW", "CRIB",
    "CROP", "CROW", "CUBE", "CURL", "CUTE", "DARE", "DARK", "DASH", "DATA", "DAWN", "DEAL", "DEAR", "DECK", "DEEP", "DEER", "DESK",
    "DICE", "DIME", "DIRT", "DISH", "DOCK", "DOME", "DOOR", "DOVE", "DOWN", "DRAW", "DROP", "DRUM", "DUCK", "DUNE", "DUSK", "DUST",
    "EAST", "EASY", "ECHO", "EDGE", "EPIC", "EVER", "FACE", "FACT", "FAIR", "FAME", "FARM", "FAST", "FAWN", "FEED", "FERN", "FILE",
    "FILM", "FIND", "FINE", "FIRE", "FIRM", "FISH", "FIVE", "FLAG", "FLAT", "FLIP", "FLOW", "FOAM", "FOLD", "FOLK", "FOOD", "FOOT",
    "FORK", "FORM", "FORT", "FOUR", "FREE", "FROG", "FUEL", "FULL", "GALE", "GAME", "GATE", "GEAR", "GIFT", "GLAD", "GLOW", "GLUE",
    "GOAL", "GOAT", "GOLD", "GOLF", "GOOD", "GRAY", "GRID", "GRIN", "GRIP", "GROW", "GULF", "GUST", "HAIR", "HALF", "HALL", "HAND",
    "HARE", "HARP", "HAWK", "HAZE", "HEAD", "HEAL", "HEAT", "HELP", "HERB", "HERD", "HERO", "HIDE", "HIGH", "HIKE", "HILL", "HINT",
    "HIVE", "HOLD", "HOME", "HOOD", "HOOF", "HOOK", "HOPE", "HORN", "HOST", "HOUR", "HUGE", "HULL", "HUNT", "HUSH", "ICON", "IDEA",
    "IRON", "ISLE", "ITEM", "JADE", "JAZZ", "JOIN", "JOKE", "JOLT", "JUMP", "KEEN", "KELP", "KIND", "KING", "KITE", "KNEE", "KNOT",
    "LACE", "LAKE", "LAMB", "LAMP", "LAND", "LANE", "LAWN", "LEAF", "LENS", "LIFT", "LIME", "LINE", "LINK", "LION", "LOAF", "LOFT",
    "LOVE", "LUCK", "LUSH", "LYNX", "MAIL", "MATE", "MAZE", "MEAL", "MENU", "MILK", "MINT", "MOON", "MOSS", "MOTH", "WAVE", "WOLF",
};
_Static_assert(sizeof(words) / sizeof(words[0]) == 256, "word index must cover one byte");

bool wifi_setup_password_generate(char output[WIFI_SETUP_PASSWORD_BYTES],
                                 const uint8_t entropy[WIFI_SETUP_PASSWORD_ENTROPY_BYTES])
{
    if (!output || !entropy) return false;
    unsigned suffix = (unsigned)entropy[2] | ((unsigned)entropy[3] << 8);
    if (suffix >= 60000) return false;
    suffix %= 10000;
    char next[WIFI_SETUP_PASSWORD_BYTES];
    memcpy(next, words[entropy[0]], 4);
    next[4] = '-';
    memcpy(next + 5, words[entropy[1]], 4);
    next[9] = '-';
    for (int i = 13; i >= 10; --i) {
        next[i] = (char)('0' + suffix % 10);
        suffix /= 10;
    }
    next[14] = 0;
    memcpy(output, next, sizeof(next));
    return true;
}

static bool known_word(const char *word)
{
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (!memcmp(word, words[i], 4)) return true;
    return false;
}

bool wifi_setup_password_valid(const char *password)
{
    if (!password) return false;
    size_t length = 0;
    while (length < WIFI_SETUP_PASSWORD_BYTES && password[length]) ++length;
    if (length != WIFI_SETUP_PASSWORD_BYTES - 1 || password[4] != '-' ||
        password[9] != '-') return false;
    for (size_t i = 10; i < 14; ++i)
        if (password[i] < '0' || password[i] > '9') return false;
    return known_word(password) && known_word(password + 5);
}
