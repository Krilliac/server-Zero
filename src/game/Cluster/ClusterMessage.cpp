#include "ClusterMessage.h"
#include <cstring>

std::vector<uint8_t> ClusterMessage::Serialize() const {
    std::vector<uint8_t> buffer;
    buffer.push_back(type);
    
    buffer.push_back(static_cast<uint8_t>(sourceNode.size()));
    buffer.insert(buffer.end(), sourceNode.begin(), sourceNode.end());
    
    buffer.push_back(static_cast<uint8_t>(targetNode.size()));
    buffer.insert(buffer.end(), targetNode.begin(), targetNode.end());
    
    buffer.insert(buffer.end(), payload.begin(), payload.end());
    return buffer;
}

ClusterMessage ClusterMessage::Deserialize(const uint8_t* data, size_t size) {
    if (size < 3) throw std::runtime_error("Invalid message size");
    
    ClusterMessage msg;
    size_t offset = 0;
    msg.type = data[offset++];
    
    uint8_t sourceLen = data[offset++];
    if (offset + sourceLen > size) throw std::runtime_error("Invalid source length");
    msg.sourceNode.assign(data + offset, data + offset + sourceLen);
    offset += sourceLen;
    
    uint8_t targetLen = data[offset++];
    if (offset + targetLen > size) throw std::runtime_error("Invalid target length");
    msg.targetNode.assign(data + offset, data + offset + targetLen);
    offset += targetLen;
    
    if (offset > size) throw std::runtime_error("Invalid payload length");
    msg.payload.assign(data + offset, data + size);
    return msg;
}