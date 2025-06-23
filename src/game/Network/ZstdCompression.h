#pragma once
#include "WorldPacket.h"
#include <zstd.h>

class ZstdCompression
{
public:
    static WorldPacket Compress(const WorldPacket& input, int compressionLevel);
    static WorldPacket Decompress(const WorldPacket& compressed);
    
private:
    static constexpr size_t MAX_COMPRESSED_SIZE = 4096;
};
