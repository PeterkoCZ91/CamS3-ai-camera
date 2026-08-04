#ifndef CZ_TEXT_H
#define CZ_TEXT_H

// Helpers for the Czech user-facing strings (Telegram messages, notification
// captions). The web UI has its own translation table in data/www/i18n.js; this
// header is only for text the firmware itself sends out.

// Czech nouns take three plural forms depending on the count, so "2 osob/y" style
// slash-hacks read badly to a native speaker. Pick the right one:
//   1        -> singular          ("1 osoba")
//   2,3,4    -> nominative plural ("3 osoby")
//   0, 5+    -> genitive plural   ("7 osob")
static inline const char* czPlural(int n, const char* one, const char* few, const char* many) {
    if (n == 1) return one;
    if (n >= 2 && n <= 4) return few;
    return many;
}

#endif // CZ_TEXT_H
