#include "data/airline_brands.h"

#include "util/callsign.h"
#include "util/icao_table.h"

namespace data::airline_brands {

const AirlineBrand* brandForCallsign(const char* callsign) {
  char prefix[4];
  if (!util::callsign::icaoPrefix(callsign, prefix)) {
    return nullptr;
  }
  return util::icao::find(kBrands, kBrandCount, prefix);
}

}  // namespace data::airline_brands
