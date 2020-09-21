/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "android.hardware.powerstats-service.pixel"

#include "include/PowerStatsAidl.h"
#include <aidl/android/hardware/powerstats/BnPowerStats.h>

#include <android-base/chrono_utils.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include <inttypes.h>
#include <chrono>
#include <numeric>
#include <string>

namespace aidl {
namespace android {
namespace hardware {
namespace powerstats {

void PowerStats::setRailDataProvider(std::unique_ptr<IRailEnergyDataProvider> p) {
    mRailEnergyDataProvider = std::move(p);
}

void PowerStats::addStateResidencyDataProvider(sp<IStateResidencyDataProvider> p) {
    int32_t id = mPowerEntityInfos.size();

    for (const auto &[entityName, states] : p->getInfo()) {
        PowerEntityInfo i = {
                .powerEntityId = id++,
                .powerEntityName = entityName,
                .states = states,
        };
        mPowerEntityInfos.emplace_back(i);
        mStateResidencyDataProviders.emplace_back(p);
    }
}

ndk::ScopedAStatus PowerStats::getEnergyData(const std::vector<int32_t> &in_railIndices,
                                             std::vector<EnergyData> *_aidl_return) {
    if (!mRailEnergyDataProvider) {
        return ndk::ScopedAStatus::ok();
    }
    return mRailEnergyDataProvider->getEnergyData(in_railIndices, _aidl_return);
}

ndk::ScopedAStatus PowerStats::getPowerEntityInfo(std::vector<PowerEntityInfo> *_aidl_return) {
    *_aidl_return = mPowerEntityInfos;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus PowerStats::getPowerEntityStateResidencyData(
        const std::vector<int32_t> &in_powerEntityIds,
        std::vector<PowerEntityStateResidencyResult> *_aidl_return) {
    // If powerEntityIds is empty then return data for all supported entities
    if (in_powerEntityIds.empty() && !mPowerEntityInfos.empty()) {
        std::vector<int32_t> v(mPowerEntityInfos.size());
        std::iota(std::begin(v), std::end(v), 0);
        return getPowerEntityStateResidencyData(v, _aidl_return);
    }

    binder_status_t err = STATUS_OK;

    std::unordered_map<std::string, std::vector<PowerEntityStateResidencyData>> stateResidencies;

    for (const int32_t id : in_powerEntityIds) {
        // skip any invalid ids
        if (id < 0 || id >= mPowerEntityInfos.size()) {
            err = STATUS_BAD_VALUE;
            continue;
        }

        // Check to see if we already have data for the given id
        std::string powerEntityName = mPowerEntityInfos[id].powerEntityName;
        if (stateResidencies.find(powerEntityName) == stateResidencies.end()) {
            mStateResidencyDataProviders[id]->getResults(&stateResidencies);
        }

        // Append results if we have them
        auto stateResidency = stateResidencies.find(powerEntityName);
        if (stateResidency != stateResidencies.end()) {
            PowerEntityStateResidencyResult res = {
                    .powerEntityId = id,
                    .stateResidencyData = stateResidency->second,
            };
            _aidl_return->emplace_back(res);
        } else {
            // We failed to retrieve results for the given id.

            // Set error code to STATUS_FAILED_TRANSACTION but don't overwrite it
            // if there is already a higher priority error code
            err = (err == STATUS_OK) ? STATUS_FAILED_TRANSACTION : err;
        }
    }

    return ndk::ScopedAStatus::fromStatus(err);
}

ndk::ScopedAStatus PowerStats::getRailInfo(std::vector<RailInfo> *_aidl_return) {
    if (!mRailEnergyDataProvider) {
        return ndk::ScopedAStatus::ok();
    }
    return mRailEnergyDataProvider->getRailInfo(_aidl_return);
}

void PowerStats::getEntityStateMaps(
        std::unordered_map<int32_t, std::string> *entityNames,
        std::unordered_map<int32_t, std::unordered_map<int32_t, std::string>> *stateNames) {
    std::vector<PowerEntityInfo> infos;
    getPowerEntityInfo(&infos);

    for (const auto &info : infos) {
        entityNames->emplace(info.powerEntityId, info.powerEntityName);
        stateNames->emplace(info.powerEntityId, std::unordered_map<int32_t, std::string>());
        auto &entityStateNames = stateNames->at(info.powerEntityId);
        for (const auto &state : info.states) {
            entityStateNames.emplace(state.powerEntityStateId, state.powerEntityStateName);
        }
    }
}

void PowerStats::getRailEnergyMaps(
        std::unordered_map<int32_t, std::pair<std::string, std::string>> *railNames) {
    std::vector<RailInfo> infos;
    getRailInfo(&infos);

    for (const auto &info : infos) {
        railNames->emplace(info.railIndex, std::make_pair(info.subsysName, info.railName));
    }
}

void PowerStats::dumpRailEnergy(std::ostringstream &oss, bool delta) {
    const char *headerFormat = "  %14s   %18s   %18s\n";
    const char *dataFormat = "  %14s   %18s   %14.2f mWs\n";
    const char *headerFormatDelta = "  %14s   %18s   %18s (%14s)\n";
    const char *dataFormatDelta = "  %14s   %18s   %14.2f mWs (%14.2f)\n";

    std::unordered_map<int32_t, std::pair<std::string, std::string>> railNames;
    getRailEnergyMaps(&railNames);

    oss << "\n============= PowerStats HAL 2.0 rail energy ==============\n";

    std::vector<EnergyData> energyData;
    getEnergyData({}, &energyData);

    if (delta) {
        static std::vector<EnergyData> prevEnergyData;
        ::android::base::boot_clock::time_point curTime = ::android::base::boot_clock::now();
        static ::android::base::boot_clock::time_point prevTime = curTime;

        oss << "Elapsed time: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(curTime - prevTime).count()
            << " ms";

        oss << ::android::base::StringPrintf(headerFormatDelta, "Subsys", "Rail",
                                             "Cumulative Energy", "Delta   ");

        std::unordered_map<int32_t, int64_t> prevEnergyDataMap;
        for (const auto &data : prevEnergyData) {
            prevEnergyDataMap.emplace(data.railIndex, data.energyUWs);
        }

        for (const auto &data : energyData) {
            const char *subsysName = railNames.at(data.railIndex).first.c_str();
            const char *railName = railNames.at(data.railIndex).second.c_str();

            auto prevEnergyDataIt = prevEnergyDataMap.find(data.railIndex);
            int64_t deltaEnergy = 0;
            if (prevEnergyDataIt != prevEnergyDataMap.end()) {
                deltaEnergy = data.energyUWs - prevEnergyDataIt->second;
            }

            oss << ::android::base::StringPrintf(dataFormatDelta, subsysName, railName,
                                                 static_cast<float>(data.energyUWs) / 1000.0,
                                                 static_cast<float>(deltaEnergy) / 1000.0);
        }

        prevEnergyData = energyData;
        prevTime = curTime;
    } else {
        oss << ::android::base::StringPrintf(headerFormat, "Subsys", "Rail", "Cumulative Energy");

        for (const auto &data : energyData) {
            oss << ::android::base::StringPrintf(dataFormat,
                                                 railNames.at(data.railIndex).first.c_str(),
                                                 railNames.at(data.railIndex).second.c_str(),
                                                 static_cast<float>(data.energyUWs) / 1000.0);
        }
    }

    oss << "========== End of PowerStats HAL 2.0 rail energy ==========\n";
}

void PowerStats::dumpStateResidency(std::ostringstream &oss, bool delta) {
    const char *headerFormat = "  %14s   %14s   %16s   %15s   %17s\n";
    const char *dataFormat =
            "  %14s   %14s   %13" PRIu64 " ms   %15" PRIu64 "   %14" PRIu64 " ms\n";
    const char *headerFormatDelta = "  %14s   %14s   %16s (%14s)   %15s (%16s)   %17s (%14s)\n";
    const char *dataFormatDelta = "  %14s   %14s   %13" PRIu64 " ms (%14" PRId64 ")   %15" PRIu64
                                  " (%16" PRId64 ")   %14" PRIu64 " ms (%14" PRId64 ")\n";

    // Construct maps to entity and state names
    std::unordered_map<int32_t, std::string> entityNames;
    std::unordered_map<int32_t, std::unordered_map<int32_t, std::string>> stateNames;
    getEntityStateMaps(&entityNames, &stateNames);

    oss << "\n============= PowerStats HAL 2.0 state residencies ==============\n";

    std::vector<PowerEntityStateResidencyResult> results;
    getPowerEntityStateResidencyData({}, &results);

    if (delta) {
        static std::vector<PowerEntityStateResidencyResult> prevResults;
        ::android::base::boot_clock::time_point curTime = ::android::base::boot_clock::now();
        static ::android::base::boot_clock::time_point prevTime = curTime;

        oss << "Elapsed time: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(curTime - prevTime).count()
            << " ms";

        oss << ::android::base::StringPrintf(headerFormatDelta, "Entity", "State", "Total time",
                                             "Delta   ", "Total entries", "Delta   ",
                                             "Last entry tstamp", "Delta ");

        // Process prevResults into a 2-tier lookup table for easy reference
        std::unordered_map<int32_t, std::unordered_map<int32_t, PowerEntityStateResidencyData>>
                prevResultsMap;
        for (const auto &prevResult : prevResults) {
            prevResultsMap.emplace(prevResult.powerEntityId,
                                   std::unordered_map<int32_t, PowerEntityStateResidencyData>());
            for (auto stateResidency : prevResult.stateResidencyData) {
                prevResultsMap.at(prevResult.powerEntityId)
                        .emplace(stateResidency.powerEntityStateId, stateResidency);
            }
        }

        // Iterate over the new result data (one "result" per entity)
        for (const auto &result : results) {
            const char *entityName = entityNames.at(result.powerEntityId).c_str();

            // Look up previous result data for the same entity
            auto prevEntityResultIt = prevResultsMap.find(result.powerEntityId);

            // Iterate over individual states within the current entity's new result
            for (const auto &stateResidency : result.stateResidencyData) {
                const char *stateName = stateNames.at(result.powerEntityId)
                                                .at(stateResidency.powerEntityStateId)
                                                .c_str();

                // If a previous result was found for the same entity, see if that
                // result also contains data for the current state
                int64_t deltaTotalTime = 0;
                int64_t deltaTotalCount = 0;
                int64_t deltaTimestamp = 0;
                if (prevEntityResultIt != prevResultsMap.end()) {
                    auto prevStateResidencyIt =
                            prevEntityResultIt->second.find(stateResidency.powerEntityStateId);
                    // If a previous result was found for the current entity and state, calculate
                    // the deltas and display them along with new result
                    if (prevStateResidencyIt != prevEntityResultIt->second.end()) {
                        deltaTotalTime = stateResidency.totalTimeInStateMs -
                                         prevStateResidencyIt->second.totalTimeInStateMs;
                        deltaTotalCount = stateResidency.totalStateEntryCount -
                                          prevStateResidencyIt->second.totalStateEntryCount;
                        deltaTimestamp = stateResidency.lastEntryTimestampMs -
                                         prevStateResidencyIt->second.lastEntryTimestampMs;
                    }
                }

                oss << ::android::base::StringPrintf(
                        dataFormatDelta, entityName, stateName, stateResidency.totalTimeInStateMs,
                        deltaTotalTime, stateResidency.totalStateEntryCount, deltaTotalCount,
                        stateResidency.lastEntryTimestampMs, deltaTimestamp);
            }
        }

        prevResults = results;
        prevTime = curTime;
    } else {
        oss << ::android::base::StringPrintf(headerFormat, "Entity", "State", "Total time",
                                             "Total entries", "Last entry tstamp");
        for (const auto &result : results) {
            for (const auto &stateResidency : result.stateResidencyData) {
                oss << ::android::base::StringPrintf(
                        dataFormat, entityNames.at(result.powerEntityId).c_str(),
                        stateNames.at(result.powerEntityId)
                                .at(stateResidency.powerEntityStateId)
                                .c_str(),
                        stateResidency.totalTimeInStateMs, stateResidency.totalStateEntryCount,
                        stateResidency.lastEntryTimestampMs);
            }
        }
    }

    oss << "========== End of PowerStats HAL 2.0 state residencies ==========\n";
}

binder_status_t PowerStats::dump(int fd, const char **args, uint32_t numArgs) {
    std::ostringstream oss;
    bool delta = (numArgs == 1) && (std::string(args[0]) == "delta");

    // Generate debug output for state residency
    dumpStateResidency(oss, delta);

    // Generate debug output for rail energy
    dumpRailEnergy(oss, delta);

    ::android::base::WriteStringToFd(oss.str(), fd);
    fsync(fd);
    return STATUS_OK;
}

}  // namespace powerstats
}  // namespace hardware
}  // namespace android
}  // namespace aidl