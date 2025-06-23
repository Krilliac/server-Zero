#include "ZstdCompression.h"
#include "Log.h"
#include <cstring>

WorldPacket ZstdCompression::Compress(const WorldPacket& input, int compressionLevel)
{
    // Skip compression for small packets
    if (input.size() <= 64) {
        return input;
    }
    
    // Create output buffer
    size_t compressBound = ZSTD_compressBound(input.size());
    std::vector<uint8_t> output(compressBound);
    
    // Perform compression
    size_t compressedSize = ZSTD_compress(
        output.data(), compressBound,
        input.contents(), input.size(),
        compressionLevel
    );
    
    if (ZSTD_isError(compressedSize)) {
        sLog.outError("Zstd compression failed: %s", ZSTD_getErrorName(compressedSize));
        return input; // Fallback to uncompressed
    }
    
    // Resize to actual compressed size
    output.resize(compressedSize);
    return WorldPacket(output.data(), compressedSize);
}

WorldPacket ZstdCompression::Decompress(const WorldPacket& compressed)
{
    // Check if packet is compressed
    if (compressed.size() < 4 || compressed.size() > MAX_COMPRESSED_SIZE) {
        return compressed; // Likely not compressed
    }
    
    // Get decompressed size from header
    uint32_t decompressedSize = *reinterpret_cast<const uint32_t*>(compressed.contents());
    
    // Create output buffer
    std::vector<uint8_t> output(decompressedSize);
    
    // Perform decompression
    size_t actualSize = ZSTD_decompress(
        output.data(), decompressedSize,
        compressed.contents() + 4, compressed.size() - 4
    );
    
    if (ZSTD_isError(actualSize)) {
        sLog.outError("Zstd decompression failed: %s", ZSTD_getErrorName(actualSize));
        return compressed; // Return original
    }
    
    return WorldPacket(output.data(), actualSize);
}
