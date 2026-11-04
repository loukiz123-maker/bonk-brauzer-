#include "bonk/html/entities.hpp"

#include <algorithm>

namespace bonk::html {
namespace {

constexpr EntityEntry kEntities[] = {
    {"AElig", 0x00C6},   {"Aacute", 0x00C1},  {"Acirc", 0x00C2},   {"Agrave", 0x00C0},
    {"Aring", 0x00C5},   {"Atilde", 0x00C3},  {"Auml", 0x00C4},    {"Ccedil", 0x00C7},
    {"ETH", 0x00D0},     {"Eacute", 0x00C9},  {"Ecirc", 0x00CA},   {"Egrave", 0x00C8},
    {"Euml", 0x00CB},    {"Iacute", 0x00CD},  {"Icirc", 0x00CE},   {"Igrave", 0x00CC},
    {"Iuml", 0x00CF},    {"Ntilde", 0x00D1},  {"Oacute", 0x00D3},  {"Ocirc", 0x00D4},
    {"Ograve", 0x00D2},  {"Oslash", 0x00D8},  {"Otilde", 0x00D5},  {"Ouml", 0x00D6},
    {"THORN", 0x00DE},   {"Uacute", 0x00DA},  {"Ucirc", 0x00DB},   {"Ugrave", 0x00D9},
    {"Uuml", 0x00DC},    {"Yacute", 0x00DD},  {"aacute", 0x00E1},  {"acirc", 0x00E2},
    {"acute", 0x00B4},   {"aelig", 0x00E6},   {"agrave", 0x00E0},  {"amp", 0x0026},
    {"aring", 0x00E5},   {"atilde", 0x00E3},  {"auml", 0x00E4},    {"brvbar", 0x00A6},
    {"ccedil", 0x00E7},  {"cedil", 0x00B8},   {"cent", 0x00A2},    {"copy", 0x00A9},
    {"curren", 0x00A4},  {"deg", 0x00B0},     {"divide", 0x00F7},  {"eacute", 0x00E9},
    {"ecirc", 0x00EA},   {"egrave", 0x00E8},  {"eth", 0x00F0},     {"euml", 0x00EB},
    {"frac12", 0x00BD},  {"frac14", 0x00BC},  {"frac34", 0x00BE},  {"gt", 0x003E},
    {"hellip", 0x2026},  {"iacute", 0x00ED},  {"icirc", 0x00EE},   {"iexcl", 0x00A1},
    {"igrave", 0x00EC},  {"iquest", 0x00BF},  {"iuml", 0x00EF},    {"laquo", 0x00AB},
    {"ldquo", 0x201C},   {"lsquo", 0x2018},   {"lt", 0x003C},      {"macr", 0x00AF},
    {"mdash", 0x2014},   {"micro", 0x00B5},   {"middot", 0x00B7},  {"nbsp", 0x00A0},
    {"ndash", 0x2013},   {"not", 0x00AC},     {"ntilde", 0x00F1},  {"oacute", 0x00F3},
    {"ocirc", 0x00F4},   {"ograve", 0x00F2},  {"ordf", 0x00AA},    {"ordm", 0x00BA},
    {"oslash", 0x00F8},  {"otilde", 0x00F5},  {"ouml", 0x00F6},    {"para", 0x00B6},
    {"plusmn", 0x00B1},  {"pound", 0x00A3},   {"quot", 0x0022},    {"raquo", 0x00BB},
    {"rdquo", 0x201D},   {"reg", 0x00AE},     {"rsquo", 0x2019},   {"sect", 0x00A7},
    {"shy", 0x00AD},     {"sup1", 0x00B9},    {"sup2", 0x00B2},    {"sup3", 0x00B3},
    {"szlig", 0x00DF},   {"thorn", 0x00FE},   {"times", 0x00D7},   {"trade", 0x2122},
    {"uacute", 0x00FA},  {"ucirc", 0x00FB},   {"ugrave", 0x00F9},  {"uml", 0x00A8},
    {"uuml", 0x00FC},    {"yacute", 0x00FD},  {"yen", 0x00A5},     {"yuml", 0x00FF},
    {"euro", 0x20AC},    {"bull", 0x2022},    {"dagger", 0x2020},  {"Dagger", 0x2021},
    {"prime", 0x2032},   {"Prime", 0x2033},   {"oline", 0x203E},   {"larr", 0x2190},
    {"uarr", 0x2191},    {"rarr", 0x2192},    {"darr", 0x2193},    {"harr", 0x2194},
    {"spades", 0x2660},  {"clubs", 0x2663},   {"hearts", 0x2665},  {"diams", 0x2666},
    {"alpha", 0x03B1},   {"beta", 0x03B2},    {"gamma", 0x03B3},   {"delta", 0x03B4},
    {"epsilon", 0x03B5}, {"lambda", 0x03BB},  {"mu", 0x03BC},      {"pi", 0x03C0},
    {"sigma", 0x03C3},   {"omega", 0x03C9},   {"Delta", 0x0394},   {"Omega", 0x03A9},
    {"apos", 0x0027},    {"Tab", 0x0009},     {"NewLine", 0x000A}, {"excl", 0x0021},
    {"num", 0x0023},     {"dollar", 0x0024},  {"percnt", 0x0025},  {"lpar", 0x0028},
    {"rpar", 0x0029},    {"ast", 0x002A},     {"plus", 0x002B},    {"comma", 0x002C},
    {"period", 0x002E},  {"sol", 0x002F},     {"colon", 0x003A},   {"semi", 0x003B},
    {"equals", 0x003D},  {"quest", 0x003F},   {"commat", 0x0040},  {"lsqb", 0x005B},
    {"rsqb", 0x005D},    {"lowbar", 0x005F},  {"grave", 0x0060},   {"lcub", 0x007B},
    {"rcub", 0x007D},    {"verbar", 0x007C},  {"tilde", 0x007E},   {"blank", 0x2423},
    {"ensp", 0x2002},    {"emsp", 0x2003},    {"thinsp", 0x2009},  {"zwnj", 0x200C},
    {"zwj", 0x200D},     {"lrm", 0x200E},     {"rlm", 0x200F},     {"sbquo", 0x201A},
    {"bdquo", 0x201E},   {"permil", 0x2030},  {"lsaquo", 0x2039},  {"rsaquo", 0x203A},
    {"check", 0x2713},   {"cross", 0x2717},   {"star", 0x2606},    {"starf", 0x2605},
    {"incare", 0x2105},  {"numero", 0x2116},  {"ohm", 0x2126},     {"infin", 0x221E},
    {"ne", 0x2260},      {"le", 0x2264},      {"ge", 0x2265},      {"sum", 0x2211},
    {"prod", 0x220F},    {"radic", 0x221A},   {"minus", 0x2212},   {"lowast", 0x2217},
    {"cap", 0x2229},     {"cup", 0x222A},     {"int", 0x222B},     {"there4", 0x2234},
    {"sim", 0x223C},     {"cong", 0x2245},    {"asymp", 0x2248},   {"equiv", 0x2261},
    {"sub", 0x2282},     {"sup", 0x2283},     {"sube", 0x2286},    {"supe", 0x2287},
    {"otimes", 0x2297},  {"perp", 0x22A5},    {"sdot", 0x22C5},    {"lceil", 0x2308},
    {"rceil", 0x2309},   {"lfloor", 0x230A},  {"rfloor", 0x230B},  {"lang", 0x27E8},
    {"rang", 0x27E9},    {"loz", 0x25CA},     {"square", 0x25A1},  {"blacksquare", 0x25A0},
    {"cir", 0x25CB},     {"bullet", 0x2022},
};

constexpr std::size_t kEntityCount = sizeof(kEntities) / sizeof(kEntities[0]);

}  // namespace

const EntityEntry* find_entity(std::string_view input, std::size_t& consumed) {
  // Entities are matched by longest name; the table is sorted, so a linear scan
  // over a 4-entry window per starting byte is enough (names are 2..10 chars).
  const EntityEntry* best = nullptr;
  std::size_t best_length = 0;
  for (std::size_t i = 0; i < kEntityCount; ++i) {
    const EntityEntry& entry = kEntities[i];
    if (entry.name.size() <= best_length) continue;
    if (input.size() < entry.name.size()) continue;
    if (input.compare(0, entry.name.size(), entry.name) != 0) continue;
    best = &entry;
    best_length = entry.name.size();
  }
  consumed = best_length;
  return best;
}

}  // namespace bonk::html
