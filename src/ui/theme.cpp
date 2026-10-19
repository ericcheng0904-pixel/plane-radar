#include "ui/theme.h"

#include "ui/radar_range.h"

namespace ui::theme {

namespace {

constexpr Style kScope = {
    /*backdrop_edge*/ {3, 6, 14},    /*backdrop_centre*/ {12, 26, 46},
    /*ring_inner*/ {24, 96, 66},     /*ring_outer*/ {44, 170, 110},
    /*spokes*/ 4,                    /*spoke*/ {18, 66, 48},
    /*tick_minor*/ {26, 92, 64},     /*tick_major*/ {48, 150, 100},
    /*double_bezel*/ false,          /*accent*/ {0, 0, 0},
    /*scale_label*/ {70, 190, 130},
    /*water*/ {16, 52, 104}, /*water_alpha*/ 150,          /*shore*/ {60, 130, 190},
    /*land_dots*/ false,             /*land_dot*/ {0, 0, 0},
    /*altitude_ramp*/ true,          /*aircraft*/ {255, 200, 60},
    /*glyph_outline*/ {0, 0, 0},
    /*tag_title*/ {255, 255, 255},   /*tag_type*/ {120, 200, 240}, /*tag_route*/ {150, 230, 200},
    /*tag_alt_ramp*/ true,           /*tag_alt*/ {255, 200, 60},
    /*trail*/ {120, 130, 145},       /*trail_end_dot*/ false,
    /*lock*/ {235, 240, 255},        /*lock_rings*/ false,
    /*card_border*/ {52, 70, 100},
    /*sweep*/ false,                 /*beam*/ {0, 0, 0},
};

constexpr Style kMap = {
    /*backdrop_edge*/ {2, 8, 34},    /*backdrop_centre*/ {6, 22, 70},
    /*ring_inner*/ {30, 80, 170},    /*ring_outer*/ {70, 140, 240},
    /*spokes*/ 12,                   /*spoke*/ {22, 60, 140},
    /*tick_minor*/ {90, 150, 240},   /*tick_major*/ {170, 210, 255},
    /*double_bezel*/ true,           /*accent*/ {230, 90, 210},
    /*scale_label*/ {140, 190, 255},
    /*water*/ {2, 10, 40}, /*water_alpha*/ 200,           /*shore*/ {40, 90, 190},
    /*land_dots*/ true,              /*land_dot*/ {50, 100, 200},
    /*altitude_ramp*/ false,         /*aircraft*/ {255, 140, 40},
    /*glyph_outline*/ {40, 12, 0},
    /*tag_title*/ {225, 240, 255},   /*tag_type*/ {130, 190, 255}, /*tag_route*/ {140, 230, 255},
    /*tag_alt_ramp*/ false,          /*tag_alt*/ {255, 190, 90},
    /*trail*/ {150, 110, 220},       /*trail_end_dot*/ true,
    /*lock*/ {255, 255, 255},        /*lock_rings*/ true,
    /*card_border*/ {60, 110, 220},
    /*sweep*/ false,                 /*beam*/ {0, 0, 0},
};

constexpr Style kClassic = {
    /*backdrop_edge*/ {0, 0, 0},     /*backdrop_centre*/ {4, 10, 8},
    /*ring_inner*/ {30, 150, 60},    /*ring_outer*/ {50, 210, 90},
    /*spokes*/ 4,                    /*spoke*/ {40, 180, 75},
    /*tick_minor*/ {30, 120, 55},    /*tick_major*/ {50, 200, 90},
    /*double_bezel*/ false,          /*accent*/ {0, 0, 0},
    /*scale_label*/ {60, 210, 100},
    /*water*/ {0, 16, 22}, /*water_alpha*/ 190,           /*shore*/ {20, 100, 90},
    /*land_dots*/ false,             /*land_dot*/ {0, 0, 0},
    /*altitude_ramp*/ false,         /*aircraft*/ {255, 90, 40},
    /*glyph_outline*/ {0, 0, 0},
    /*tag_title*/ {255, 255, 255},   /*tag_type*/ {110, 210, 255}, /*tag_route*/ {130, 255, 170},
    /*tag_alt_ramp*/ false,          /*tag_alt*/ {255, 215, 90},
    /*trail*/ {235, 235, 245},       /*trail_end_dot*/ false,
    /*lock*/ {255, 255, 255},        /*lock_rings*/ false,
    /*card_border*/ {40, 160, 80},
    /*sweep*/ false,                 /*beam*/ {0, 0, 0},
};

constexpr Style kSweep = {
    /*backdrop_edge*/ {0, 4, 2},     /*backdrop_centre*/ {2, 14, 8},
    /*ring_inner*/ {20, 90, 40},     /*ring_outer*/ {60, 220, 100},
    /*spokes*/ 4,                    /*spoke*/ {20, 90, 40},
    /*tick_minor*/ {20, 90, 40},     /*tick_major*/ {50, 170, 80},
    /*double_bezel*/ false,          /*accent*/ {0, 0, 0},
    /*scale_label*/ {60, 200, 100},
    /*water*/ {0, 14, 10}, /*water_alpha*/ 190,           /*shore*/ {16, 80, 44},
    /*land_dots*/ false,             /*land_dot*/ {0, 0, 0},
    /*altitude_ramp*/ false,         /*aircraft*/ {150, 255, 150},
    /*glyph_outline*/ {0, 0, 0},
    /*tag_title*/ {150, 255, 160},   /*tag_type*/ {90, 200, 110}, /*tag_route*/ {150, 255, 160},
    /*tag_alt_ramp*/ false,          /*tag_alt*/ {90, 200, 110},
    /*trail*/ {40, 140, 70},         /*trail_end_dot*/ false,
    /*lock*/ {190, 255, 190},        /*lock_rings*/ true,
    /*card_border*/ {40, 150, 70},
    /*sweep*/ true,                  /*beam*/ {120, 255, 140},
};

}  // namespace

Id currentId() {
  const uint8_t t = radar::theme();
  return t < radar::kThemeCount ? static_cast<Id>(t) : Id::kScope;
}

const Style& current() {
  switch (currentId()) {
    case Id::kMap:
      return kMap;
    case Id::kClassic:
      return kClassic;
    case Id::kSweep:
      return kSweep;
    case Id::kScope:
    default:
      return kScope;
  }
}

}  // namespace ui::theme
