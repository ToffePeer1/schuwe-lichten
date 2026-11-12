#include "library_store.h"

#include <LittleFS.h>

#include <cstring>

namespace library_store {

namespace {

constexpr char PRESETS_PATH[] = "/presets.bin";
constexpr char SHOWS_PATH[] = "/shows.bin";
constexpr size_t BUFFER_SIZE = 2048;
static_assert(BUFFER_SIZE >= 1 + 32 * (1 + 33 + 10) + 4, "presets blob must fit");
static_assert(BUFFER_SIZE >= 1 + 16 * (1 + 33 + 1 + 24 * 3) + 4, "shows blob must fit");

// Loop task only, so static buffers are safe and keep the stack small.
uint8_t writeBuffer[BUFFER_SIZE];
uint8_t readBuffer[BUFFER_SIZE];
bool isMounted = false;

size_t readFile(const char* path, uint8_t* out, size_t size) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    return 0;
  }
  const size_t length = file.size();
  const size_t read = length <= size ? file.read(out, length) : 0;
  file.close();
  return read == length ? length : 0;
}

bool writeVerified(const char* path, const uint8_t* data, size_t size) {
  if (!isMounted) {
    return false;
  }
  char tmpPath[24];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  File file = LittleFS.open(tmpPath, "w");
  if (!file) {
    return false;
  }
  const size_t written = file.write(data, size);
  file.close();
  if (written != size || readFile(tmpPath, readBuffer, sizeof(readBuffer)) != size ||
      std::memcmp(readBuffer, data, size) != 0) {
    LittleFS.remove(tmpPath);
    return false;
  }
  return LittleFS.rename(tmpPath, path);  // littlefs replaces the old file atomically
}

// Missing file = empty slots (not an error). Present but invalid = reset.
bool loadFile(const char* path, size_t expected, bool (*decode)(const uint8_t*, size_t, laser::Library*),
              laser::Library* library) {
  if (!LittleFS.exists(path)) {
    return false;
  }
  const size_t size = readFile(path, readBuffer, sizeof(readBuffer));
  return size != expected || !decode(readBuffer, size, library);
}

}  // namespace

LoadResult load(laser::Library* library) {
  *library = laser::Library{};
  isMounted = LittleFS.begin(true);
  if (!isMounted) {
    return {false, false, false, 0};
  }
  const bool wasPresetsReset = loadFile(PRESETS_PATH, laser::presetsBlobSize(), laser::decodePresets, library);
  const bool wasShowsReset = loadFile(SHOWS_PATH, laser::showsBlobSize(), laser::decodeShows, library);
  const uint8_t droppedShows = laser::dropDanglingShows(*library);
  return {true, wasPresetsReset, wasShowsReset || droppedShows > 0, droppedShows};
}

bool savePresets(const laser::Library& library) {
  const size_t size = laser::encodePresets(library, writeBuffer, sizeof(writeBuffer));
  return size > 0 && writeVerified(PRESETS_PATH, writeBuffer, size);
}

bool saveShows(const laser::Library& library) {
  const size_t size = laser::encodeShows(library, writeBuffer, sizeof(writeBuffer));
  return size > 0 && writeVerified(SHOWS_PATH, writeBuffer, size);
}

}  // namespace library_store
