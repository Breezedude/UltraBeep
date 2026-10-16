"""Text preparation for Chatterbox.

Chatterbox reads digits unreliably: in the benchmark "1230 Meter" became "230 Meter" plus invented
words, "-100" lost its sign and "47" became "740", while the same numbers written as words were
right. The voice pack texts keep digits (each voice reads them in its own language), so the worker
spells them out before synthesis.
"""
import re

from num2words import num2words

# Locale of a voice -> Chatterbox language id (Norwegian Bokmal is "no" there).
LANGUAGES = {"ar", "da", "de", "el", "en", "es", "fi", "fr", "he", "hi", "it", "ja", "ko", "ms", "nl", "no",
             "pl", "pt", "ru", "sv", "sw", "tr", "zh"}
LOCALE_LANGUAGE = {"nb": "no", "nn": "no"}

# Words num2words does not get right on its own: the sign (Polish fails on negatives) and the decimal
# mark, after which every digit is read singly ("eins Komma zwei fünf"). Languages missing here keep
# their decimals and negatives as digits.
MINUS = {"de": "minus", "en": "minus", "fr": "moins", "it": "meno", "es": "menos", "pt": "menos", "nl": "min",
         "pl": "minus", "sv": "minus", "fi": "miinus", "da": "minus", "no": "minus", "tr": "eksi", "ru": "минус",
         "ja": "マイナス", "ko": "마이너스", "he": "מינוס"}
DECIMAL = {"de": "Komma", "en": "point", "fr": "virgule", "it": "virgola", "es": "coma", "pt": "vírgula",
           "nl": "komma", "pl": "przecinek", "sv": "komma", "fi": "pilkku", "da": "komma", "no": "komma",
           "tr": "virgül", "ru": "запятая", "ja": "点", "ko": "점", "he": "נקודה"}

# An optional sign, digits, and a decimal part after "," or "."; not inside words or longer numbers.
NUMBER = re.compile(r"(?<![\w.,])([-−]?)(\d+)(?:[.,](\d+))?(?![\w])")


def language_of(locale):
    """The Chatterbox language of a locale ("de-AT" -> "de"), or None when Chatterbox lacks it."""
    lang = locale.split("-")[0].lower()
    lang = LOCALE_LANGUAGE.get(lang, lang)
    return lang if lang in LANGUAGES else None


def spell_numbers(text, lang):
    """`text` with its numbers written as words in `lang`; numbers num2words cannot say stay digits."""

    def words(digits):
        return num2words(int(digits), lang=lang)

    def replace(m):
        sign, whole, frac = m.groups()
        try:
            spoken = words(whole)
            if frac:
                spoken += f" {DECIMAL[lang]} " + " ".join(words(d) for d in frac)
            if sign:
                spoken = f"{MINUS[lang]} {spoken}"
            return spoken
        except (NotImplementedError, KeyError, ValueError, OverflowError):
            return m.group(0)

    return NUMBER.sub(replace, text)


def prepare(text, lang):
    """The text as Chatterbox should get it: whitespace collapsed, numbers as words."""
    return spell_numbers(" ".join(text.split()), lang)


def plausible_seconds(text):
    """The longest a clip of `text` (as spoken) should take. Chatterbox sometimes rambles on after the
    text or garbles a long word; such a result is longer, and is generated again. Measured: 1.0 s for
    "Abflug", 1.3 s for "GPS ok", 2.5 s for "zweiundvierzig Kilometer pro Stunde" (garbled: 4.4 s),
    3.2 to 3.9 s for the 64 characters of the longest catalogue line, 4.5 s for a "1230 Meter" with an
    invented tail."""
    return 1.2 + 0.07 * len(text)
