#include "SessionParser.hpp"

#include <functional>
#include <Random.hpp>
#include <SafePtr.hpp>

namespace elastos {

/* =========================================== */
/* === static variables initialize =========== */
/* =========================================== */

/* =========================================== */
/* === static function implement ============= */
/* =========================================== */

/* =========================================== */
/* === class public function implement  ====== */
/* =========================================== */
void SessionParser::config(const std::filesystem::path& cacheDir,
                           std::shared_ptr<OnSectionListener> listener)
{
    this->bodyCacheDir = cacheDir;
    this->onSectionListener = listener;
}

int SessionParser::dispose(const std::vector<uint8_t>& data)
{
    Log::W(Log::TAG, "%s datasize=%d", __PRETTY_FUNCTION__, data.size());

    auto dataPos = 0;

    do {
        int ret = parseProtocol(data, dataPos);
        if(ret == ErrCode::CarrierSessionDataNotEnough) {
            return 0; // Ignore to dump backtrace
        }
        CHECK_ERROR(ret);
        dataPos += ret;

        ret = savePayload(data, dataPos);
        CHECK_ERROR(ret);
        dataPos += ret;
        Log::D(Log::TAG, "%s datapos=%d", __PRETTY_FUNCTION__, dataPos);
    } while(dataPos < data.size());

    return 0;
}

/* =========================================== */
/* === class protected function implement  === */
/* =========================================== */


/* =========================================== */
/* === class private function implement  ===== */
/* =========================================== */
int SessionParser::parseProtocol(const std::vector<uint8_t>& data, int offset)
{
     // protocal info has been parsed, value data is body payload, return directly.
    if(protocol != nullptr
    && protocol->info.headSize == protocol->payload->headData.size()) {
        Log::D(Log::TAG, "Protocol has been parsed.");
        return 0;
    }

    auto cachingDataPrevSize = cachingData.size();
    cachingData.insert(cachingData.end(), data.begin() + offset, data.end());

    if(protocol == nullptr) {
        // find first magic number and remove garbage data.
        auto searchMagicNum = hton(Protocol::MagicNumber);
        int garbageIdx;
        for(garbageIdx = 0; garbageIdx <= static_cast<int>(cachingData.size() - sizeof(Protocol::Info)); garbageIdx++) {
            if(searchMagicNum == *((typeof(Protocol::Info::magicNumber)*)(cachingData.data() + garbageIdx))) {
                break;
            }
        }
        if(garbageIdx > 0) {
            Log::W(Log::TAG, "Remove garbage size %d", garbageIdx);
            cachingData.erase(cachingData.begin(), cachingData.begin() + garbageIdx);
            cachingDataPrevSize -= garbageIdx; // recompute valid data of previous cache.
        }

        // return and parse next time if data is not enough to parse info.
        if(cachingData.size() < sizeof(Protocol::Info)) {
            Log::D(Log::TAG, "Protocol info data is not enough.");
            return ErrCode::CarrierSessionDataNotEnough;
        }

        protocol = std::make_unique<Protocol>();
        protocol->payload = std::make_unique<Protocol::Payload>(bodyCacheDir);

        auto dataPtr = cachingData.data();

        auto netOrderMagicNum = *((typeof(protocol->info.magicNumber)*)(dataPtr));
        protocol->info.magicNumber = ntoh(netOrderMagicNum);
        dataPtr += sizeof(protocol->info.magicNumber);

        auto netOrderVersion = *((typeof(protocol->info.version)*)(dataPtr));
        protocol->info.version = ntoh(netOrderVersion);
        if(protocol->info.version != Protocol::Version_01_00_00) {
            Log::W(Log::TAG, "Unsupperted version %u", protocol->info.version);
            return ErrCode::CarrierSessionUnsuppertedVersion;
        }
        dataPtr += sizeof(protocol->info.version);

        auto netOrderHeadSize = *((typeof(protocol->info.headSize)*)(dataPtr));
        protocol->info.headSize = ntoh(netOrderHeadSize);
        dataPtr += sizeof(protocol->info.headSize);

        auto netOrderBodySize = *((typeof(protocol->info.bodySize)*)(dataPtr));
        protocol->info.bodySize = ntoh(netOrderBodySize);
        dataPtr += sizeof(protocol->info.bodySize);
    }

    // return and parse next time if data is not enough to save as head data.
    if(cachingData.size() < (sizeof(protocol->info) + protocol->info.headSize)) {
        Log::D(Log::TAG, "Protocol head data is not enough.");
        return ErrCode::CarrierSessionDataNotEnough;
    }

    // store head data and clear cache.
    auto headDataPtr = cachingData.data() + sizeof(protocol->info);
    protocol->payload->headData = {headDataPtr, headDataPtr + protocol->info.headSize};
    cachingData.clear();

    // body offset of input data.
    auto bodyStartIdx = (sizeof(protocol->info) + protocol->info.headSize - cachingDataPrevSize);

    return bodyStartIdx;
}

int SessionParser::savePayload(const std::vector<uint8_t>& data, int offset)
{
    auto neededData = protocol->info.bodySize - protocol->payload->bodyData.receivedBodySize;
    auto realSize = (neededData < (data.size() - offset)
                  ? neededData : (data.size() - offset));

    protocol->payload->bodyData.stream.write((char*)data.data() + offset, realSize);
    // protocol->payload.bodyData.insert(protocol->payload.bodyData.end(),
    //                                   data.begin() + offset, data.begin() + offset + realSize);
    protocol->payload->bodyData.receivedBodySize += realSize;

    if(protocol->payload->bodyData.receivedBodySize == protocol->info.bodySize) {
        if(onSectionListener != nullptr) {
            protocol->payload->bodyData.stream.flush();
            protocol->payload->bodyData.stream.close();

            (*onSectionListener)(protocol->payload->headData, protocol->payload->bodyData.cacheName);
        }
        protocol.reset();
    }

    return realSize;
}

uint64_t SessionParser::ntoh(uint64_t value) const 
{
    // uint64_t rval;
    // uint8_t *data = (uint8_t*)&rval;

    // data[0] = value >> 56;
    // data[1] = value >> 48;
    // data[2] = value >> 40;
    // data[3] = value >> 32;
    // data[4] = value >> 24;
    // data[5] = value >> 16;
    // data[6] = value >> 8;
    // data[7] = value >> 0;

    // return rval;
    return ntohll(value);
}

uint64_t SessionParser::hton(uint64_t value) const 
{
    // return ntoh(value);
    return htonll(value);
}

uint32_t SessionParser::ntoh(uint32_t value) const
{
    // uint32_t rval;
    // uint8_t *data = (uint8_t*)&rval;

    // data[0] = value >> 24;
    // data[1] = value >> 16;
    // data[2] = value >> 8;
    // data[3] = value >> 0;

    // return rval;
    return ntohl(value);
}

uint32_t SessionParser::hton(uint32_t value) const 
{
    // return ntoh(value);
    return htonl(value);
}

SessionParser::Protocol::Payload::Payload(const std::filesystem::path& bodyCacheDir)
    : headData()
    , bodyData()
{
    bodyData.cacheName = bodyCacheDir / (CacheName + std::to_string(Random::Gen<uint32_t>()));
    bodyData.stream.open(bodyData.cacheName, std::ios::binary);
    bodyData.receivedBodySize = 0;
    Log::W(Log::TAG, "%s body data cache: %s", __PRETTY_FUNCTION__, bodyData.cacheName.c_str());
}

SessionParser::Protocol::Payload::~Payload()
{
    bodyData.stream.close();
    // std::filesystem::remove(bodyData.cacheName); // TODO
    Log::W(Log::TAG, "%s", __PRETTY_FUNCTION__);
}


} // namespace elastos