// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file FullHistoryMerger.cxx
/// \brief Implementation of O2 Mergers, v0.1
///
/// \author Piotr Konopka, piotr.jan.konopka@cern.ch

#include "Mergers/FullHistoryMerger.h"
#include "Mergers/MergerAlgorithm.h"
#include "Mergers/MergerBuilder.h"
#include "Mergers/MergeInterface.h"

#include "Headers/DataHeader.h"
#include "Framework/InputRecordWalker.h"
#include "Framework/DataRefUtils.h"
#include "Framework/Logger.h"
#include <Monitoring/MonitoringFactory.h>
#include <InfoLogger/InfoLogger.hxx>

using namespace o2::header;
using namespace o2::framework;
using namespace std::chrono;

namespace o2::mergers
{

FullHistoryMerger::FullHistoryMerger(const MergerConfig& config, const header::DataHeader::SubSpecificationType& subSpec)
  : mConfig(config),
    mSubSpec(subSpec)
{
}

FullHistoryMerger::~FullHistoryMerger()
{
  delete mFirstObjectSerialized.second.header;
  delete mFirstObjectSerialized.second.payload;
  delete mFirstObjectSerialized.second.spec;
}

void FullHistoryMerger::init(framework::InitContext& ictx)
{
  mCyclesSinceReset = 0;
  mCollector = monitoring::MonitoringFactory::Get(mConfig.monitoringUrl);
  mCollector->addGlobalTag(monitoring::tags::Key::Subsystem, monitoring::tags::Value::Mergers);

  // clear the state before starting the run, especially important for START->STOP->START sequence
  ictx.services().get<CallbackService>().set<CallbackService::Id::Start>([this]() { clear(); });

  // set detector field in infologger
  try {
    auto& ilContext = ictx.services().get<AliceO2::InfoLogger::InfoLoggerContext>();
    ilContext.setField(AliceO2::InfoLogger::InfoLoggerContext::FieldName::Detector, mConfig.detectorName);
  } catch (const RuntimeErrorRef& err) {
    LOG(warn) << "Could not find the DPL InfoLogger Context.";
  }
}

void FullHistoryMerger::run(framework::ProcessingContext& ctx)
{
  // we have to avoid mistaking the timer input with data inputs.
  auto* timerHeader = ctx.inputs().get("timer-publish").header;

  for (const DataRef& ref : InputRecordWalker(ctx.inputs())) {
    if (ref.header != timerHeader) {
      updateCache(ref);
      mUpdatesReceived++;
    }
  }

  if (ctx.inputs().isValid("timer-publish") && !mFirstObjectSerialized.first.empty()) {
    mCyclesSinceReset++;
    mergeCache();
    publish(ctx.outputs());

    if (mConfig.mergedObjectTimespan.value == MergedObjectTimespan::LastDifference ||
        mConfig.mergedObjectTimespan.value == MergedObjectTimespan::NCycles && mConfig.mergedObjectTimespan.param == mCyclesSinceReset) {
      clear();
    }
  }
}

void FullHistoryMerger::endOfStream(framework::EndOfStreamContext& eosContext)
{
  mergeCache();
  publish(eosContext.outputs());
}

// I am not calling it reset(), because it does not have to be performed during the FairMQs reset.
void FullHistoryMerger::clear()
{
  mFirstObjectSerialized.first.clear();
  delete mFirstObjectSerialized.second.header;
  delete mFirstObjectSerialized.second.payload;
  delete mFirstObjectSerialized.second.spec;
  mFirstObjectSerialized.second.header = nullptr;
  mFirstObjectSerialized.second.payload = nullptr;
  mFirstObjectSerialized.second.spec = nullptr;
  mMergedObject = std::monostate{};
  mCache.clear();
  mCyclesSinceReset = 0;
  mTotalObjectsMerged = 0;
  mObjectsMerged = 0;
  mTotalUpdatesReceived = 0;
  mUpdatesReceived = 0;
}

void FullHistoryMerger::updateCache(const DataRef& ref)
{
  auto* dh = DataRefUtils::getHeader<DataHeader*>(ref);
  auto payloadSize = DataRefUtils::getPayloadSize(ref);
  std::string sourceID = std::string(dh->dataOrigin.str) + "/" + std::string(dh->dataDescription.str) + "/" + std::to_string(dh->subSpecification);

  // I am not sure if ref.spec is always a concrete spec and not a broader matcher. Comparing it this way should be safer.
  if (mFirstObjectSerialized.first.empty() || mFirstObjectSerialized.first == sourceID) {
    // We store one object in the serialized form, so we can take it as the first object to be merged (multiple times).
    // If we kept it deserialized, we would need to require implementing a clone() method in MergeInterface.
    LOG(debug) << "Received the first input object in the run or after the last moving window reset";

    delete mFirstObjectSerialized.second.spec;
    delete mFirstObjectSerialized.second.header;
    delete mFirstObjectSerialized.second.payload;

    mFirstObjectSerialized.first = sourceID;
    mFirstObjectSerialized.second.spec = new InputSpec(*ref.spec);
    mFirstObjectSerialized.second.header = new char[Stack::headerStackSize(reinterpret_cast<std::byte const*>(dh))];
    memcpy((void*)mFirstObjectSerialized.second.header, ref.header, dh->headerSize);
    mFirstObjectSerialized.second.payload = new char[payloadSize];
    memcpy((void*)mFirstObjectSerialized.second.payload, ref.payload, payloadSize);

  } else {
    mCache[sourceID] = object_store_helpers::extractObjectFrom(ref);
  }
}

void FullHistoryMerger::mergeCache()
{
  LOG(debug) << "Merging " << mCache.size() + 1 << " objects.";

  if (mFirstObjectSerialized.second.payload == nullptr) {
    // no objects arrived to the Merger yet, nothing to use.
    return;
  }

  mMergedObject = object_store_helpers::extractObjectFrom(mFirstObjectSerialized.second);
  assert(!std::holds_alternative<std::monostate>(mMergedObject));
  mObjectsMerged++;

  // We expect that all the objects use the same kind of interface
  if (std::holds_alternative<TObjectPtr>(mMergedObject)) {
    auto target = std::get<TObjectPtr>(mMergedObject);
    for (auto& [name, entry] : mCache) {
      (void)name;
      auto other = std::get<TObjectPtr>(entry);
      algorithm::merge(target.get(), other.get());
      mObjectsMerged++;
    }

  } else if (std::holds_alternative<MergeInterfacePtr>(mMergedObject)) {
    auto target = std::get<MergeInterfacePtr>(mMergedObject);
    for (auto& [name, entry] : mCache) {
      (void)name;
      auto other = std::get<MergeInterfacePtr>(entry);
      target->merge(other.get());
      mObjectsMerged++;
    }

  } else if (std::holds_alternative<VectorOfTObjectPtrs>(mMergedObject)) {
    auto target = std::get<VectorOfTObjectPtrs>(mMergedObject);
    for (auto& [_, entry] : mCache) {
      auto other = std::get<VectorOfTObjectPtrs>(entry);
      algorithm::merge(target, other);
      mObjectsMerged += target.size();
    }
  }
}

void FullHistoryMerger::publish(framework::DataAllocator& allocator)
{
  if (std::holds_alternative<std::monostate>(mMergedObject)) {
    LOG(info) << "No objects received since start or reset, nothing to publish";
  } else if (object_store_helpers::snapshot(allocator, mSubSpec, mMergedObject)) {
    LOG(info) << "Published the merged object containing " << mCache.size() + 1 << " incomplete objects. "
              << mUpdatesReceived << " updates were received during the last cycle.";
  } else {
    throw std::runtime_error("mMergedObjectIntegral' variant has no value.");
  }

  mTotalObjectsMerged += mObjectsMerged;
  mTotalUpdatesReceived += mUpdatesReceived;
  mCollector->send({mTotalObjectsMerged, "total_objects_merged"}, monitoring::DerivedMetricMode::RATE);
  mCollector->send({mObjectsMerged, "objects_merged_since_last_publication"});
  mCollector->send({mTotalUpdatesReceived, "total_updates_received"}, monitoring::DerivedMetricMode::RATE);
  mCollector->send({mUpdatesReceived, "updates_received_since_last_publication"});
  mCollector->send({mCyclesSinceReset, "cycles_since_reset"});
  mObjectsMerged = 0;
  mUpdatesReceived = 0;
}

} // namespace o2::mergers
