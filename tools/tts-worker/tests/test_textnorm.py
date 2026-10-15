"""python -m unittest discover -s tests (from tools/tts-worker, in the worker's venv)"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from textnorm import language_of, plausible_seconds, prepare, spell_numbers  # noqa: E402


class SpellNumbers(unittest.TestCase):
    def test_catalogue_lines_german(self):
        self.assertEqual(spell_numbers("1230 Meter", "de"), "eintausendzweihundertdreißig Meter")
        self.assertEqual(spell_numbers("-100 Meter", "de"), "minus einhundert Meter")
        self.assertEqual(spell_numbers("minus 1,5 Meter pro Sekunde", "de"), "minus eins Komma fünf Meter pro Sekunde")
        self.assertEqual(spell_numbers("47 Kilometer pro Stunde", "de"), "siebenundvierzig Kilometer pro Stunde")
        self.assertEqual(spell_numbers("0 Uhr", "de"), "null Uhr")

    def test_other_languages(self):
        self.assertEqual(spell_numbers("1.5 metres per second", "en"), "one point five metres per second")
        self.assertEqual(spell_numbers("-10 mètres", "fr"), "moins dix mètres")
        self.assertEqual(spell_numbers("-20 metrów", "pl"), "minus dwadzieścia metrów")

    def test_words_and_unknown_languages_unchanged(self):
        self.assertEqual(spell_numbers("GPS ok", "de"), "GPS ok")
        self.assertEqual(spell_numbers("Profil 1", "de"), "Profil eins")
        self.assertEqual(spell_numbers("v2 und A320", "de"), "v2 und A320")
        self.assertEqual(spell_numbers("12 μέτρα", "el"), "12 μέτρα")  # num2words has no Greek

    def test_prepare_collapses_whitespace(self):
        self.assertEqual(prepare("  Batterie:\tschwach ", "de"), "Batterie: schwach")


class Languages(unittest.TestCase):
    def test_language_of(self):
        self.assertEqual(language_of("de-DE"), "de")
        self.assertEqual(language_of("nb-NO"), "no")
        self.assertEqual(language_of("pt-BR"), "pt")
        self.assertIsNone(language_of("cs-CZ"))

    def test_plausible_seconds(self):
        self.assertGreater(plausible_seconds("Abflug"), 1.0)
        self.assertGreater(plausible_seconds("Halte die Taste zum Zurücksetzen auf Werkseinstellungen gedrückt"), 3.9)
        self.assertLess(plausible_seconds("eintausendzweihundertdreißig Meter"), 4.5 + 0.5)
        self.assertLess(plausible_seconds("1230 Meter"), 4.5)


if __name__ == "__main__":
    unittest.main()
