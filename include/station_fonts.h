#pragma once

#include <Adafruit_GFX.h>

// Rôles typographiques de l'écran. STATION_FONT_THEME choisit la famille :
//   0 = FreeSans d'Adafruit GFX (ASCII : textes affichés sans accents)
//   1 = Barlow Condensed, convertie depuis Google Fonts (licence OFL, voir
//       fonts/OFL-BarlowCondensed.txt) en Latin-1 : accents et signe degré.
// Régénération : tools/fonts/make_fonts.sh.
#ifndef STATION_FONT_THEME
#define STATION_FONT_THEME 1
#endif

#if STATION_FONT_THEME == 1
#include "fonts/BarlowCondensed_Medium11pt8b.h"
#include "fonts/BarlowCondensed_SemiBold11pt8b.h"
#include "fonts/BarlowCondensed_Medium14pt8b.h"
#include "fonts/BarlowCondensed_SemiBold14pt8b.h"
#include "fonts/BarlowCondensed_Bold25pt8b.h"
#include "fonts/BarlowCondensed_Bold48pt7b.h"
#include "fonts/BarlowCondensed_Bold56pt7b.h"

const GFXfont* const FONT_SMALL = &BarlowCondensed_Medium11pt8b;
const GFXfont* const FONT_SMALL_BOLD = &BarlowCondensed_SemiBold11pt8b;
const GFXfont* const FONT_MEDIUM = &BarlowCondensed_Medium14pt8b;
const GFXfont* const FONT_MEDIUM_BOLD = &BarlowCondensed_SemiBold14pt8b;
const GFXfont* const FONT_LARGE = &BarlowCondensed_Bold25pt8b;
// Horloges : chiffres et ':' seulement, une taille par disposition d'écran.
const GFXfont* const FONT_CLOCK_LARGE = &BarlowCondensed_Bold48pt7b;
const GFXfont* const FONT_CLOCK_XL = &BarlowCondensed_Bold56pt7b;
#else
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

const GFXfont* const FONT_SMALL = &FreeSans9pt7b;
const GFXfont* const FONT_SMALL_BOLD = &FreeSansBold9pt7b;
const GFXfont* const FONT_MEDIUM = &FreeSans12pt7b;
const GFXfont* const FONT_MEDIUM_BOLD = &FreeSansBold12pt7b;
const GFXfont* const FONT_LARGE = &FreeSansBold24pt7b;
const GFXfont* const FONT_CLOCK_LARGE = &FreeSansBold24pt7b;
const GFXfont* const FONT_CLOCK_XL = &FreeSansBold24pt7b;
#endif
