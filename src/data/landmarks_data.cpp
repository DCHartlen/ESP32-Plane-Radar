#include "data/landmarks.h"

namespace data::landmarks {

// The generated tables (kChunkData, kDeltaData) if scripts/build_landmarks.py has been run.
// The file is git-ignored because it shows where the sites are.
#if __has_include("landmarks_local.inc")
#include "landmarks_local.inc"

const Chunk* const kChunks = kChunkData;
const size_t kChunkCount = sizeof(kChunkData) / sizeof(kChunkData[0]);
const Delta* const kDeltas = kDeltaData;
#else
const Chunk* const kChunks = nullptr;
const size_t kChunkCount = 0;
const Delta* const kDeltas = nullptr;
#endif

}  // namespace data::landmarks
