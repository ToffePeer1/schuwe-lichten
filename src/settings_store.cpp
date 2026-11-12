#include "settings_store.h"

#include <Preferences.h>

#include <cstring>

namespace settings_store {

namespace {

constexpr char NAMESPACE[] = "laser";
constexpr char KEY[] = "settings";

}  // namespace

LoadResult load(const char* factoryPin, uint16_t defaultAddress) {
  LoadResult result{laser::defaultSettings(factoryPin, defaultAddress), true, false};
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, true)) {
    return result;  // namespace doesn't exist yet: first boot
  }
  const size_t stored = prefs.getBytesLength(KEY);
  if (stored == 0) {
    prefs.end();
    return result;
  }
  uint8_t blob[laser::SETTINGS_BLOB_SIZE] = {};
  const size_t read = stored == sizeof(blob) ? prefs.getBytes(KEY, blob, sizeof(blob)) : 0;
  prefs.end();
  result.wasMissing = false;
  laser::Settings decoded;
  if (read == sizeof(blob) && laser::decodeSettings(blob, sizeof(blob), &decoded)) {
    result.settings = decoded;
  } else {
    result.wasReset = true;
  }
  return result;
}

bool save(const laser::Settings& settings) {
  uint8_t blob[laser::SETTINGS_BLOB_SIZE];
  if (laser::encodeSettings(settings, blob, sizeof(blob)) != sizeof(blob)) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(NAMESPACE, false)) {
    return false;
  }
  const bool isWritten = prefs.putBytes(KEY, blob, sizeof(blob)) == sizeof(blob);
  uint8_t check[laser::SETTINGS_BLOB_SIZE] = {};
  const bool isReadBack = isWritten && prefs.getBytes(KEY, check, sizeof(check)) == sizeof(check);
  prefs.end();
  return isReadBack && std::memcmp(blob, check, sizeof(blob)) == 0;
}

}  // namespace settings_store
