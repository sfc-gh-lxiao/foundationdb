/*
 * DataDistribution.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <set>
#include <sstream>
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/Knobs.h"
#include "fdbclient/StorageServerInterface.h"
#include "fdbclient/SystemData.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/RunTransaction.actor.h"
#include "fdbrpc/Replication.h"
#include "fdbserver/DataDistribution.actor.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/TLogInterface.h"
#include "fdbserver/WaitFailure.h"
#include "flow/ActorCollection.h"
#include "flow/Arena.h"
#include "flow/BooleanParam.h"
#include "flow/Trace.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // This must be the last #include.
#include "flow/serialize.h"

class TCTeamInfo;
struct TCMachineInfo;
class TCMachineTeamInfo;

namespace {

// Helper function for STL containers, with flow-friendly error handling
template <class MapContainer, class K>
auto get(MapContainer& m, K const& k) -> decltype(m.at(k)) {
	auto it = m.find(k);
	ASSERT(it != m.end());
	return it->second;
}

} // namespace

FDB_BOOLEAN_PARAM(IsPrimary);

ACTOR Future<Void> checkAndRemoveInvalidLocalityAddr(DDTeamCollection* self);
ACTOR Future<Void> removeWrongStoreType(DDTeamCollection* self);
ACTOR Future<Void> waitForAllDataRemoved(Database cx, UID serverID, Version addedVersion, DDTeamCollection* teams);
bool _exclusionSafetyCheck(std::vector<UID>& excludeServerIDs, DDTeamCollection* teamCollection);

struct TCServerInfo : public ReferenceCounted<TCServerInfo> {
	UID id;
	Version addedVersion; // Read version when this Server is added
	DDTeamCollection* collection;
	StorageServerInterface lastKnownInterface;
	ProcessClass lastKnownClass;
	std::vector<Reference<TCTeamInfo>> teams;
	Reference<TCMachineInfo> machine;
	Future<Void> tracker;
	int64_t dataInFlightToServer;
	ErrorOr<GetStorageMetricsReply> serverMetrics;
	Promise<std::pair<StorageServerInterface, ProcessClass>> interfaceChanged;
	Future<std::pair<StorageServerInterface, ProcessClass>> onInterfaceChanged;
	Promise<Void> removed;
	Future<Void> onRemoved;
	Future<Void> onTSSPairRemoved;
	Promise<Void> killTss;
	Promise<Void> wakeUpTracker;
	bool inDesiredDC;
	LocalityEntry localityEntry;
	Promise<Void> updated;
	AsyncVar<bool> wrongStoreTypeToRemove;
	AsyncVar<bool> ssVersionTooFarBehind;
	// A storage server's StoreType does not change.
	// To change storeType for an ip:port, we destroy the old one and create a new one.
	KeyValueStoreType storeType; // Storage engine type

	TCServerInfo(StorageServerInterface ssi,
	             DDTeamCollection* collection,
	             ProcessClass processClass,
	             bool inDesiredDC,
	             Reference<LocalitySet> storageServerSet,
	             Version addedVersion = 0)
	  : id(ssi.id()), addedVersion(addedVersion), collection(collection), lastKnownInterface(ssi),
	    lastKnownClass(processClass), dataInFlightToServer(0), onInterfaceChanged(interfaceChanged.getFuture()),
	    onRemoved(removed.getFuture()), onTSSPairRemoved(Never()), inDesiredDC(inDesiredDC),
	    storeType(KeyValueStoreType::END) {

		if (!ssi.isTss()) {
			localityEntry = ((LocalityMap<UID>*)storageServerSet.getPtr())->add(ssi.locality, &id);
		}
	}

	bool isCorrectStoreType(KeyValueStoreType configStoreType) const {
		// A new storage server's store type may not be set immediately.
		// If a storage server does not reply its storeType, it will be tracked by failure monitor and removed.
		return (storeType == configStoreType || storeType == KeyValueStoreType::END);
	}
	~TCServerInfo();
};

class TCMachineInfo : public ReferenceCounted<TCMachineInfo> {
	TCMachineInfo() = default;

public:
	std::vector<Reference<TCServerInfo>> serversOnMachine; // SOMEDAY: change from vector to set
	Standalone<StringRef> machineID;
	std::vector<Reference<TCMachineTeamInfo>> machineTeams; // SOMEDAY: split good and bad machine teams.
	LocalityEntry localityEntry;

	Reference<TCMachineInfo> clone() const {
		auto result = Reference<TCMachineInfo>(new TCMachineInfo);
		result->serversOnMachine = serversOnMachine;
		result->machineID = machineID;
		result->machineTeams = machineTeams;
		result->localityEntry = localityEntry;
		return result;
	}

	explicit TCMachineInfo(Reference<TCServerInfo> server, const LocalityEntry& entry) : localityEntry(entry) {
		ASSERT(serversOnMachine.empty());
		serversOnMachine.push_back(server);

		LocalityData& locality = server->lastKnownInterface.locality;
		ASSERT(locality.zoneId().present());
		machineID = locality.zoneId().get();
	}

	std::string getServersIDStr() const {
		std::stringstream ss;
		if (serversOnMachine.empty())
			return "[unset]";

		for (const auto& server : serversOnMachine) {
			ss << server->id.toString() << " ";
		}

		return std::move(ss).str();
	}
};

ACTOR Future<Void> updateServerMetrics(Reference<TCServerInfo> server);

// TeamCollection's machine team information
class TCMachineTeamInfo : public ReferenceCounted<TCMachineTeamInfo> {
public:
	std::vector<Reference<TCMachineInfo>> machines;
	std::vector<Standalone<StringRef>> machineIDs;
	std::vector<Reference<TCTeamInfo>> serverTeams;
	UID id;

	explicit TCMachineTeamInfo(std::vector<Reference<TCMachineInfo>> const& machines)
	  : machines(machines), id(deterministicRandom()->randomUniqueID()) {
		machineIDs.reserve(machines.size());
		for (int i = 0; i < machines.size(); i++) {
			machineIDs.push_back(machines[i]->machineID);
		}
		sort(machineIDs.begin(), machineIDs.end());
	}

	int size() const {
		ASSERT(machines.size() == machineIDs.size());
		return machineIDs.size();
	}

	std::string getMachineIDsStr() const {
		std::stringstream ss;

		if (machineIDs.empty())
			return "[unset]";

		for (const auto& id : machineIDs) {
			ss << id.contents().toString() << " ";
		}

		return std::move(ss).str();
	}

	bool operator==(TCMachineTeamInfo& rhs) const { return this->machineIDs == rhs.machineIDs; }
};

// TeamCollection's server team info.
class TCTeamInfo final : public ReferenceCounted<TCTeamInfo>, public IDataDistributionTeam {
	std::vector<Reference<TCServerInfo>> servers;
	std::vector<UID> serverIDs;
	bool healthy;
	bool wrongConfiguration; // True if any of the servers in the team have the wrong configuration
	int priority;
	UID id;

public:
	Reference<TCMachineTeamInfo> machineTeam;
	Future<Void> tracker;

	explicit TCTeamInfo(std::vector<Reference<TCServerInfo>> const& servers)
	  : servers(servers), healthy(true), wrongConfiguration(false), priority(SERVER_KNOBS->PRIORITY_TEAM_HEALTHY),
	    id(deterministicRandom()->randomUniqueID()) {
		if (servers.empty()) {
			TraceEvent(SevInfo, "ConstructTCTeamFromEmptyServers").log();
		}
		serverIDs.reserve(servers.size());
		for (int i = 0; i < servers.size(); i++) {
			serverIDs.push_back(servers[i]->id);
		}
	}

	std::string getTeamID() const override { return id.shortString(); }

	std::vector<StorageServerInterface> getLastKnownServerInterfaces() const override {
		std::vector<StorageServerInterface> v;
		v.reserve(servers.size());
		for (const auto& server : servers) {
			v.push_back(server->lastKnownInterface);
		}
		return v;
	}
	int size() const override {
		ASSERT(servers.size() == serverIDs.size());
		return servers.size();
	}
	std::vector<UID> const& getServerIDs() const override { return serverIDs; }
	const std::vector<Reference<TCServerInfo>>& getServers() const { return servers; }

	std::string getServerIDsStr() const {
		std::stringstream ss;

		if (serverIDs.empty())
			return "[unset]";

		for (const auto& id : serverIDs) {
			ss << id.toString() << " ";
		}

		return std::move(ss).str();
	}

	void addDataInFlightToTeam(int64_t delta) override {
		for (int i = 0; i < servers.size(); i++)
			servers[i]->dataInFlightToServer += delta;
	}
	int64_t getDataInFlightToTeam() const override {
		int64_t dataInFlight = 0.0;
		for (int i = 0; i < servers.size(); i++)
			dataInFlight += servers[i]->dataInFlightToServer;
		return dataInFlight;
	}

	int64_t getLoadBytes(bool includeInFlight = true, double inflightPenalty = 1.0) const override {
		int64_t physicalBytes = getLoadAverage();
		double minAvailableSpaceRatio = getMinAvailableSpaceRatio(includeInFlight);
		int64_t inFlightBytes = includeInFlight ? getDataInFlightToTeam() / servers.size() : 0;
		double availableSpaceMultiplier =
		    SERVER_KNOBS->AVAILABLE_SPACE_RATIO_CUTOFF /
		    (std::max(std::min(SERVER_KNOBS->AVAILABLE_SPACE_RATIO_CUTOFF, minAvailableSpaceRatio), 0.000001));
		if (servers.size() > 2) {
			// make sure in triple replication the penalty is high enough that you will always avoid a team with a
			// member at 20% free space
			availableSpaceMultiplier = availableSpaceMultiplier * availableSpaceMultiplier;
		}

		if (minAvailableSpaceRatio < SERVER_KNOBS->TARGET_AVAILABLE_SPACE_RATIO) {
			TraceEvent(SevWarn, "DiskNearCapacity")
			    .suppressFor(1.0)
			    .detail("AvailableSpaceRatio", minAvailableSpaceRatio);
		}

		return (physicalBytes + (inflightPenalty * inFlightBytes)) * availableSpaceMultiplier;
	}

	int64_t getMinAvailableSpace(bool includeInFlight = true) const override {
		int64_t minAvailableSpace = std::numeric_limits<int64_t>::max();
		for (const auto& server : servers) {
			if (server->serverMetrics.present()) {
				auto& replyValue = server->serverMetrics.get();

				ASSERT(replyValue.available.bytes >= 0);
				ASSERT(replyValue.capacity.bytes >= 0);

				int64_t bytesAvailable = replyValue.available.bytes;
				if (includeInFlight) {
					bytesAvailable -= server->dataInFlightToServer;
				}

				minAvailableSpace = std::min(bytesAvailable, minAvailableSpace);
			}
		}

		return minAvailableSpace; // Could be negative
	}

	double getMinAvailableSpaceRatio(bool includeInFlight = true) const override {
		double minRatio = 1.0;
		for (const auto& server : servers) {
			if (server->serverMetrics.present()) {
				auto& replyValue = server->serverMetrics.get();

				ASSERT(replyValue.available.bytes >= 0);
				ASSERT(replyValue.capacity.bytes >= 0);

				int64_t bytesAvailable = replyValue.available.bytes;
				if (includeInFlight) {
					bytesAvailable = std::max((int64_t)0, bytesAvailable - server->dataInFlightToServer);
				}

				if (replyValue.capacity.bytes == 0)
					minRatio = 0;
				else
					minRatio = std::min(minRatio, ((double)bytesAvailable) / replyValue.capacity.bytes);
			}
		}

		return minRatio;
	}

	bool hasHealthyAvailableSpace(double minRatio) const override {
		return getMinAvailableSpaceRatio() >= minRatio && getMinAvailableSpace() > SERVER_KNOBS->MIN_AVAILABLE_SPACE;
	}

	Future<Void> updateStorageMetrics() override { return doUpdateStorageMetrics(this); }

	bool isOptimal() const override {
		for (const auto& server : servers) {
			if (server->lastKnownClass.machineClassFitness(ProcessClass::Storage) > ProcessClass::UnsetFit) {
				return false;
			}
		}
		return true;
	}

	bool isWrongConfiguration() const override { return wrongConfiguration; }
	void setWrongConfiguration(bool wrongConfiguration) override { this->wrongConfiguration = wrongConfiguration; }
	bool isHealthy() const override { return healthy; }
	void setHealthy(bool h) override { healthy = h; }
	int getPriority() const override { return priority; }
	void setPriority(int p) override { priority = p; }
	void addref() override { ReferenceCounted<TCTeamInfo>::addref(); }
	void delref() override { ReferenceCounted<TCTeamInfo>::delref(); }

	void addServers(const std::vector<UID>& servers) override {
		serverIDs.reserve(servers.size());
		for (int i = 0; i < servers.size(); i++) {
			serverIDs.push_back(servers[i]);
		}
	}

private:
	// Calculate an "average" of the metrics replies that we received.  Penalize teams from which we did not receive all
	// replies.
	int64_t getLoadAverage() const {
		int64_t bytesSum = 0;
		int added = 0;
		for (int i = 0; i < servers.size(); i++)
			if (servers[i]->serverMetrics.present()) {
				added++;
				bytesSum += servers[i]->serverMetrics.get().load.bytes;
			}

		if (added < servers.size())
			bytesSum *= 2;

		return added == 0 ? 0 : bytesSum / added;
	}

	// Calculate the max of the metrics replies that we received.

	ACTOR Future<Void> doUpdateStorageMetrics(TCTeamInfo* self) {
		std::vector<Future<Void>> updates;
		updates.reserve(self->servers.size());
		for (int i = 0; i < self->servers.size(); i++)
			updates.push_back(updateServerMetrics(self->servers[i]));
		wait(waitForAll(updates));
		return Void();
	}
};

struct ServerStatus {
	bool isWiggling;
	bool isFailed;
	bool isUndesired;
	bool isWrongConfiguration;
	bool initialized; // AsyncMap erases default constructed objects
	LocalityData locality;
	ServerStatus()
	  : isWiggling(false), isFailed(true), isUndesired(false), isWrongConfiguration(false), initialized(false) {}
	ServerStatus(bool isFailed, bool isUndesired, bool isWiggling, LocalityData const& locality)
	  : isWiggling(isWiggling), isFailed(isFailed), isUndesired(isUndesired), isWrongConfiguration(false),
	    initialized(true), locality(locality) {}
	bool isUnhealthy() const { return isFailed || isUndesired; }
	const char* toString() const {
		return isFailed ? "Failed" : isUndesired ? "Undesired" : isWiggling ? "Wiggling" : "Healthy";
	}

	bool operator==(ServerStatus const& r) const {
		return isFailed == r.isFailed && isUndesired == r.isUndesired && isWiggling == r.isWiggling &&
		       isWrongConfiguration == r.isWrongConfiguration && locality == r.locality && initialized == r.initialized;
	}
	bool operator!=(ServerStatus const& r) const { return !(*this == r); }

	// If a process has reappeared without the storage server that was on it (isFailed == true), we don't need to
	// exclude it We also don't need to exclude processes who are in the wrong configuration (since those servers will
	// be removed)
	bool excludeOnRecruit() const { return !isFailed && !isWrongConfiguration; }
};
typedef AsyncMap<UID, ServerStatus> ServerStatusMap;

// Read keyservers, return unique set of teams
ACTOR Future<Reference<InitialDataDistribution>> getInitialDataDistribution(Database cx,
                                                                            UID distributorId,
                                                                            MoveKeysLock moveKeysLock,
                                                                            std::vector<Optional<Key>> remoteDcIds,
                                                                            const DDEnabledState* ddEnabledState) {
	state Reference<InitialDataDistribution> result = makeReference<InitialDataDistribution>();
	state Key beginKey = allKeys.begin;

	state bool succeeded;

	state Transaction tr(cx);

	state std::map<UID, Optional<Key>> server_dc;
	state std::map<std::vector<UID>, std::pair<std::vector<UID>, std::vector<UID>>> team_cache;
	state std::vector<std::pair<StorageServerInterface, ProcessClass>> tss_servers;

	// Get the server list in its own try/catch block since it modifies result.  We don't want a subsequent failure
	// causing entries to be duplicated
	loop {
		server_dc.clear();
		succeeded = false;
		try {

			// Read healthyZone value which is later used to determine on/off of failure triggered DD
			tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			Optional<Value> val = wait(tr.get(healthyZoneKey));
			if (val.present()) {
				auto p = decodeHealthyZoneValue(val.get());
				if (p.second > tr.getReadVersion().get() || p.first == ignoreSSFailuresZoneString) {
					result->initHealthyZoneValue = Optional<Key>(p.first);
				} else {
					result->initHealthyZoneValue = Optional<Key>();
				}
			} else {
				result->initHealthyZoneValue = Optional<Key>();
			}

			result->mode = 1;
			tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				rd >> result->mode;
			}
			if (!result->mode || !ddEnabledState->isDDEnabled()) {
				// DD can be disabled persistently (result->mode = 0) or transiently (isDDEnabled() = 0)
				TraceEvent(SevDebug, "GetInitialDataDistribution_DisabledDD").log();
				return result;
			}

			state Future<std::vector<ProcessData>> workers = getWorkers(&tr);
			state Future<RangeResult> serverList = tr.getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY);
			wait(success(workers) && success(serverList));
			ASSERT(!serverList.get().more && serverList.get().size() < CLIENT_KNOBS->TOO_MANY);

			std::map<Optional<Standalone<StringRef>>, ProcessData> id_data;
			for (int i = 0; i < workers.get().size(); i++)
				id_data[workers.get()[i].locality.processId()] = workers.get()[i];

			succeeded = true;

			for (int i = 0; i < serverList.get().size(); i++) {
				auto ssi = decodeServerListValue(serverList.get()[i].value);
				if (!ssi.isTss()) {
					result->allServers.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
					server_dc[ssi.id()] = ssi.locality.dcId();
				} else {
					tss_servers.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
				}
			}

			break;
		} catch (Error& e) {
			wait(tr.onError(e));

			ASSERT(!succeeded); // We shouldn't be retrying if we have already started modifying result in this loop
			TraceEvent("GetInitialTeamsRetry", distributorId).log();
		}
	}

	// If keyServers is too large to read in a single transaction, then we will have to break this process up into
	// multiple transactions. In that case, each iteration should begin where the previous left off
	while (beginKey < allKeys.end) {
		TEST(beginKey > allKeys.begin); // Multi-transactional getInitialDataDistribution
		loop {
			succeeded = false;
			try {
				tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
				wait(checkMoveKeysLockReadOnly(&tr, moveKeysLock, ddEnabledState));
				state RangeResult UIDtoTagMap = wait(tr.getRange(serverTagKeys, CLIENT_KNOBS->TOO_MANY));
				ASSERT(!UIDtoTagMap.more && UIDtoTagMap.size() < CLIENT_KNOBS->TOO_MANY);
				RangeResult keyServers = wait(krmGetRanges(&tr,
				                                           keyServersPrefix,
				                                           KeyRangeRef(beginKey, allKeys.end),
				                                           SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT,
				                                           SERVER_KNOBS->MOVE_KEYS_KRM_LIMIT_BYTES));
				succeeded = true;

				std::vector<UID> src, dest, last;

				// for each range
				for (int i = 0; i < keyServers.size() - 1; i++) {
					DDShardInfo info(keyServers[i].key);
					decodeKeyServersValue(UIDtoTagMap, keyServers[i].value, src, dest);
					if (remoteDcIds.size()) {
						auto srcIter = team_cache.find(src);
						if (srcIter == team_cache.end()) {
							for (auto& id : src) {
								auto& dc = server_dc[id];
								if (std::find(remoteDcIds.begin(), remoteDcIds.end(), dc) != remoteDcIds.end()) {
									info.remoteSrc.push_back(id);
								} else {
									info.primarySrc.push_back(id);
								}
							}
							result->primaryTeams.insert(info.primarySrc);
							result->remoteTeams.insert(info.remoteSrc);
							team_cache[src] = std::make_pair(info.primarySrc, info.remoteSrc);
						} else {
							info.primarySrc = srcIter->second.first;
							info.remoteSrc = srcIter->second.second;
						}
						if (dest.size()) {
							info.hasDest = true;
							auto destIter = team_cache.find(dest);
							if (destIter == team_cache.end()) {
								for (auto& id : dest) {
									auto& dc = server_dc[id];
									if (std::find(remoteDcIds.begin(), remoteDcIds.end(), dc) != remoteDcIds.end()) {
										info.remoteDest.push_back(id);
									} else {
										info.primaryDest.push_back(id);
									}
								}
								result->primaryTeams.insert(info.primaryDest);
								result->remoteTeams.insert(info.remoteDest);
								team_cache[dest] = std::make_pair(info.primaryDest, info.remoteDest);
							} else {
								info.primaryDest = destIter->second.first;
								info.remoteDest = destIter->second.second;
							}
						}
					} else {
						info.primarySrc = src;
						auto srcIter = team_cache.find(src);
						if (srcIter == team_cache.end()) {
							result->primaryTeams.insert(src);
							team_cache[src] = std::pair<std::vector<UID>, std::vector<UID>>();
						}
						if (dest.size()) {
							info.hasDest = true;
							info.primaryDest = dest;
							auto destIter = team_cache.find(dest);
							if (destIter == team_cache.end()) {
								result->primaryTeams.insert(dest);
								team_cache[dest] = std::pair<std::vector<UID>, std::vector<UID>>();
							}
						}
					}
					result->shards.push_back(info);
				}

				ASSERT(keyServers.size() > 0);
				beginKey = keyServers.end()[-1].key;
				break;
			} catch (Error& e) {
				TraceEvent("GetInitialTeamsKeyServersRetry", distributorId).error(e);

				wait(tr.onError(e));
				ASSERT(!succeeded); // We shouldn't be retrying if we have already started modifying result in this loop
			}
		}

		tr.reset();
	}

	// a dummy shard at the end with no keys or servers makes life easier for trackInitialShards()
	result->shards.push_back(DDShardInfo(allKeys.end));

	// add tss to server list AFTER teams are built
	for (auto& it : tss_servers) {
		result->allServers.push_back(it);
	}

	return result;
}

ACTOR Future<Void> storageServerTracker(struct DDTeamCollection* self,
                                        Database cx,
                                        TCServerInfo* server,
                                        Promise<Void> errorOut,
                                        Version addedVersion,
                                        const DDEnabledState* ddEnabledState,
                                        bool isTss);

Future<Void> teamTracker(struct DDTeamCollection* const& self,
                         Reference<TCTeamInfo> const& team,
                         bool const& badTeam,
                         bool const& redundantTeam);

struct DDTeamCollection : ReferenceCounted<DDTeamCollection> {
	// clang-format off
	enum class Status { NONE = 0, WIGGLING = 1, EXCLUDED = 2, FAILED = 3};

	// addActor: add to actorCollection so that when an actor has error, the ActorCollection can catch the error.
	// addActor is used to create the actorCollection when the dataDistributionTeamCollection is created
	PromiseStream<Future<Void>> addActor;
	Database cx;
	UID distributorId;
	DatabaseConfiguration configuration;

	bool doBuildTeams;
	bool lastBuildTeamsFailed;
	Future<Void> teamBuilder;
	AsyncTrigger restartTeamBuilder;

	MoveKeysLock lock;
	PromiseStream<RelocateShard> output;
	std::vector<UID> allServers;
	ServerStatusMap server_status;
	int64_t unhealthyServers;
	std::map<int,int> priority_teams;
	std::map<UID, Reference<TCServerInfo>> server_info;
	std::map<Key, std::vector<Reference<TCServerInfo>>> pid2server_info; // some process may serve as multiple storage servers
	std::vector<AddressExclusion> wiggle_addresses; // collection of wiggling servers' address
	std::map<UID, Reference<TCServerInfo>> tss_info_by_pair;
	std::map<UID, Reference<TCServerInfo>> server_and_tss_info; // TODO could replace this with an efficient way to do a read-only concatenation of 2 data structures? 
	std::map<Key, int> lagging_zones; // zone to number of storage servers lagging
	AsyncVar<bool> disableFailingLaggingServers;
	Optional<Key> wigglingPid; // Process id of current wiggling storage server;
    Reference<AsyncVar<bool>> pauseWiggle;

	// machine_info has all machines info; key must be unique across processes on the same machine
	std::map<Standalone<StringRef>, Reference<TCMachineInfo>> machine_info;
	std::vector<Reference<TCMachineTeamInfo>> machineTeams; // all machine teams
	LocalityMap<UID> machineLocalityMap; // locality info of machines

	std::vector<Reference<TCTeamInfo>> teams;
	std::vector<Reference<TCTeamInfo>> badTeams;
	Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure;
	PromiseStream<UID> removedServers;
	PromiseStream<UID> removedTSS;
	std::set<UID> recruitingIds; // The IDs of the SS/TSS which are being recruited
	std::set<NetworkAddress> recruitingLocalities;
	Future<Void> initialFailureReactionDelay;
	Future<Void> initializationDoneActor;
	Promise<Void> serverTrackerErrorOut;
	AsyncVar<int> recruitingStream;
	Debouncer restartRecruiting;

	int healthyTeamCount;
	Reference<AsyncVar<bool>> zeroHealthyTeams;

	int optimalTeamCount;
	AsyncVar<bool> zeroOptimalTeams;

	int bestTeamKeepStuckCount = 0;

	bool isTssRecruiting; // If tss recruiting is waiting on a pair, don't consider DD recruiting for the purposes of QuietDB

	// WIGGLING if an address is under storage wiggling.
	// EXCLUDED if an address is in the excluded list in the database.
	// FAILED if an address is permanently failed.
	// NONE by default.  Updated asynchronously (eventually)
	AsyncMap< AddressExclusion, Status > excludedServers;

	std::set<AddressExclusion> invalidLocalityAddr; // These address have invalidLocality for the configured storagePolicy

	std::vector<Optional<Key>> includedDCs;
	Optional<std::vector<Optional<Key>>> otherTrackedDCs;
	bool primary;
	Reference<AsyncVar<bool>> processingUnhealthy;
	Future<Void> readyToStart;
	Future<Void> checkTeamDelay;
	Promise<Void> addSubsetComplete;
	Future<Void> badTeamRemover;
	Future<Void> checkInvalidLocalities;

	Future<Void> wrongStoreTypeRemover;

	Reference<LocalitySet> storageServerSet;

	std::vector<DDTeamCollection*> teamCollections;
	AsyncVar<Optional<Key>> healthyZone;
	Future<bool> clearHealthyZoneFuture;
	double medianAvailableSpace;
	double lastMedianAvailableSpaceUpdate;
	// clang-format on

	int lowestUtilizationTeam;
	int highestUtilizationTeam;

	AsyncTrigger printDetailedTeamsInfo;
	PromiseStream<GetMetricsRequest> getShardMetrics;
	PromiseStream<Promise<int>> getUnhealthyRelocationCount;
	Promise<UID> removeFailedServer;

	void resetLocalitySet() {
		storageServerSet = Reference<LocalitySet>(new LocalityMap<UID>());
		LocalityMap<UID>* storageServerMap = (LocalityMap<UID>*)storageServerSet.getPtr();

		for (auto& it : server_info) {
			it.second->localityEntry = storageServerMap->add(it.second->lastKnownInterface.locality, &it.second->id);
		}
	}

	bool satisfiesPolicy(const std::vector<Reference<TCServerInfo>>& team, int amount = -1) const {
		std::vector<LocalityEntry> forcedEntries, resultEntries;
		if (amount == -1) {
			amount = team.size();
		}

		forcedEntries.reserve(amount);
		for (int i = 0; i < amount; i++) {
			forcedEntries.push_back(team[i]->localityEntry);
		}

		bool result = storageServerSet->selectReplicas(configuration.storagePolicy, forcedEntries, resultEntries);
		return result && resultEntries.size() == 0;
	}

	DDTeamCollection(Database const& cx,
	                 UID distributorId,
	                 MoveKeysLock const& lock,
	                 PromiseStream<RelocateShard> const& output,
	                 Reference<ShardsAffectedByTeamFailure> const& shardsAffectedByTeamFailure,
	                 DatabaseConfiguration configuration,
	                 std::vector<Optional<Key>> includedDCs,
	                 Optional<std::vector<Optional<Key>>> otherTrackedDCs,
	                 Future<Void> readyToStart,
	                 Reference<AsyncVar<bool>> zeroHealthyTeams,
	                 IsPrimary primary,
	                 Reference<AsyncVar<bool>> processingUnhealthy,
	                 PromiseStream<GetMetricsRequest> getShardMetrics,
	                 Promise<UID> removeFailedServer,
	                 PromiseStream<Promise<int>> getUnhealthyRelocationCount)
	  : cx(cx), distributorId(distributorId), configuration(configuration), doBuildTeams(true),
	    lastBuildTeamsFailed(false), teamBuilder(Void()), lock(lock), output(output), unhealthyServers(0),
	    shardsAffectedByTeamFailure(shardsAffectedByTeamFailure),
	    initialFailureReactionDelay(
	        delayed(readyToStart, SERVER_KNOBS->INITIAL_FAILURE_REACTION_DELAY, TaskPriority::DataDistribution)),
	    initializationDoneActor(logOnCompletion(readyToStart && initialFailureReactionDelay, this)),
	    recruitingStream(0), restartRecruiting(SERVER_KNOBS->DEBOUNCE_RECRUITING_DELAY), healthyTeamCount(0),
	    zeroHealthyTeams(zeroHealthyTeams), optimalTeamCount(0), zeroOptimalTeams(true), isTssRecruiting(false),
	    includedDCs(includedDCs), otherTrackedDCs(otherTrackedDCs), primary(primary),
	    processingUnhealthy(processingUnhealthy), readyToStart(readyToStart),
	    checkTeamDelay(delay(SERVER_KNOBS->CHECK_TEAM_DELAY, TaskPriority::DataDistribution)), badTeamRemover(Void()),
	    checkInvalidLocalities(Void()), wrongStoreTypeRemover(Void()), storageServerSet(new LocalityMap<UID>()),
	    clearHealthyZoneFuture(true), medianAvailableSpace(SERVER_KNOBS->MIN_AVAILABLE_SPACE_RATIO),
	    lastMedianAvailableSpaceUpdate(0), lowestUtilizationTeam(0), highestUtilizationTeam(0),
	    getShardMetrics(getShardMetrics), getUnhealthyRelocationCount(getUnhealthyRelocationCount),
	    removeFailedServer(removeFailedServer) {
		if (!primary || configuration.usableRegions == 1) {
			TraceEvent("DDTrackerStarting", distributorId).detail("State", "Inactive").trackLatest("DDTrackerStarting");
		}
	}

	~DDTeamCollection() {
		TraceEvent("DDTeamCollectionDestructed", distributorId).detail("Primary", primary);

		// Cancel the teamBuilder to avoid creating new teams after teams are cancelled.
		teamBuilder.cancel();
		// TraceEvent("DDTeamCollectionDestructed", distributorId)
		//    .detail("Primary", primary)
		//    .detail("TeamBuilderDestroyed", server_info.size());

		// Other teamCollections also hold pointer to this teamCollection;
		// TeamTracker may access the destructed DDTeamCollection if we do not reset the pointer
		for (int i = 0; i < teamCollections.size(); i++) {
			if (teamCollections[i] != nullptr && teamCollections[i] != this) {
				for (int j = 0; j < teamCollections[i]->teamCollections.size(); ++j) {
					if (teamCollections[i]->teamCollections[j] == this) {
						teamCollections[i]->teamCollections[j] = nullptr;
					}
				}
			}
		}
		// Team tracker has pointers to DDTeamCollections both in primary and remote.
		// The following kills a reference cycle between the teamTracker actor and the TCTeamInfo that both holds and is
		// held by the actor It also ensures that the trackers are done fiddling with healthyTeamCount before we free
		// this
		for (auto& team : teams) {
			team->tracker.cancel();
		}
		// The commented TraceEvent log is useful in detecting what is running during the destruction
		// TraceEvent("DDTeamCollectionDestructed", distributorId)
		//     .detail("Primary", primary)
		//     .detail("TeamTrackerDestroyed", teams.size());
		for (auto& badTeam : badTeams) {
			badTeam->tracker.cancel();
		}
		// TraceEvent("DDTeamCollectionDestructed", distributorId)
		//     .detail("Primary", primary)
		//     .detail("BadTeamTrackerDestroyed", badTeams.size());
		// The following makes sure that, even if a reference to a team is held in the DD Queue, the tracker will be
		// stopped
		//  before the server_status map to which it has a pointer, is destroyed.
		for (auto& [_, info] : server_and_tss_info) {
			info->tracker.cancel();
			info->collection = nullptr;
		}

		// TraceEvent("DDTeamCollectionDestructed", distributorId)
		//    .detail("Primary", primary)
		//    .detail("ServerTrackerDestroyed", server_info.size());
	}

	void addLaggingStorageServer(Key zoneId) {
		lagging_zones[zoneId]++;
		if (lagging_zones.size() > std::max(1, configuration.storageTeamSize - 1) &&
		    !disableFailingLaggingServers.get())
			disableFailingLaggingServers.set(true);
	}

	void removeLaggingStorageServer(Key zoneId) {
		auto iter = lagging_zones.find(zoneId);
		ASSERT(iter != lagging_zones.end());
		iter->second--;
		ASSERT(iter->second >= 0);
		if (iter->second == 0)
			lagging_zones.erase(iter);
		if (lagging_zones.size() <= std::max(1, configuration.storageTeamSize - 1) &&
		    disableFailingLaggingServers.get())
			disableFailingLaggingServers.set(false);
	}

	ACTOR static Future<Void> logOnCompletion(Future<Void> signal, DDTeamCollection* self) {
		wait(signal);
		wait(delay(SERVER_KNOBS->LOG_ON_COMPLETION_DELAY, TaskPriority::DataDistribution));

		if (!self->primary || self->configuration.usableRegions == 1) {
			TraceEvent("DDTrackerStarting", self->distributorId)
			    .detail("State", "Active")
			    .trackLatest("DDTrackerStarting");
		}

		return Void();
	}

	ACTOR static Future<Void> interruptableBuildTeams(DDTeamCollection* self) {
		if (!self->addSubsetComplete.isSet()) {
			wait(addSubsetOfEmergencyTeams(self));
			self->addSubsetComplete.send(Void());
		}

		loop {
			choose {
				when(wait(self->buildTeams(self))) { return Void(); }
				when(wait(self->restartTeamBuilder.onTrigger())) {}
			}
		}
	}

	ACTOR static Future<Void> checkBuildTeams(DDTeamCollection* self) {
		wait(self->checkTeamDelay);
		while (!self->teamBuilder.isReady())
			wait(self->teamBuilder);

		if (self->doBuildTeams && self->readyToStart.isReady()) {
			self->doBuildTeams = false;
			self->teamBuilder = self->interruptableBuildTeams(self);
			wait(self->teamBuilder);
		}

		return Void();
	}

	// SOMEDAY: Make bestTeam better about deciding to leave a shard where it is (e.g. in PRIORITY_TEAM_HEALTHY case)
	//		    use keys, src, dest, metrics, priority, system load, etc.. to decide...
	ACTOR static Future<Void> getTeam(DDTeamCollection* self, GetTeamRequest req) {
		try {
			wait(self->checkBuildTeams(self));
			if (now() - self->lastMedianAvailableSpaceUpdate > SERVER_KNOBS->AVAILABLE_SPACE_UPDATE_DELAY) {
				self->lastMedianAvailableSpaceUpdate = now();
				std::vector<double> teamAvailableSpace;
				teamAvailableSpace.reserve(self->teams.size());
				for (const auto& team : self->teams) {
					if (team->isHealthy()) {
						teamAvailableSpace.push_back(team->getMinAvailableSpaceRatio());
					}
				}

				size_t pivot = teamAvailableSpace.size() / 2;
				if (teamAvailableSpace.size() > 1) {
					std::nth_element(
					    teamAvailableSpace.begin(), teamAvailableSpace.begin() + pivot, teamAvailableSpace.end());
					self->medianAvailableSpace =
					    std::max(SERVER_KNOBS->MIN_AVAILABLE_SPACE_RATIO,
					             std::min(SERVER_KNOBS->TARGET_AVAILABLE_SPACE_RATIO, teamAvailableSpace[pivot]));
				} else {
					self->medianAvailableSpace = SERVER_KNOBS->MIN_AVAILABLE_SPACE_RATIO;
				}
				if (self->medianAvailableSpace < SERVER_KNOBS->TARGET_AVAILABLE_SPACE_RATIO) {
					TraceEvent(SevWarn, "DDTeamMedianAvailableSpaceTooSmall", self->distributorId)
					    .detail("MedianAvailableSpaceRatio", self->medianAvailableSpace)
					    .detail("TargetAvailableSpaceRatio", SERVER_KNOBS->TARGET_AVAILABLE_SPACE_RATIO)
					    .detail("Primary", self->primary);
					self->printDetailedTeamsInfo.trigger();
				}
			}

			bool foundSrc = false;
			for (int i = 0; i < req.src.size(); i++) {
				if (self->server_info.count(req.src[i])) {
					foundSrc = true;
					break;
				}
			}

			// Select the best team
			// Currently the metric is minimum used disk space (adjusted for data in flight)
			// Only healthy teams may be selected. The team has to be healthy at the moment we update
			//   shardsAffectedByTeamFailure or we could be dropping a shard on the floor (since team
			//   tracking is "edge triggered")
			// SOMEDAY: Account for capacity, load (when shardMetrics load is high)

			// self->teams.size() can be 0 under the ConfigureTest.txt test when we change configurations
			// The situation happens rarely. We may want to eliminate this situation someday
			if (!self->teams.size()) {
				req.reply.send(std::make_pair(Optional<Reference<IDataDistributionTeam>>(), foundSrc));
				return Void();
			}

			int64_t bestLoadBytes = 0;
			Optional<Reference<IDataDistributionTeam>> bestOption;
			std::vector<Reference<IDataDistributionTeam>> randomTeams;
			const std::set<UID> completeSources(req.completeSources.begin(), req.completeSources.end());

			// Note: this block does not apply any filters from the request
			if (!req.wantsNewServers) {
				for (int i = 0; i < req.completeSources.size(); i++) {
					if (!self->server_info.count(req.completeSources[i])) {
						continue;
					}
					auto& teamList = self->server_info[req.completeSources[i]]->teams;
					for (int j = 0; j < teamList.size(); j++) {
						bool found = true;
						auto serverIDs = teamList[j]->getServerIDs();
						for (int k = 0; k < teamList[j]->size(); k++) {
							if (!completeSources.count(serverIDs[k])) {
								found = false;
								break;
							}
						}
						if (found && teamList[j]->isHealthy()) {
							bestOption = teamList[j];
							req.reply.send(std::make_pair(bestOption, foundSrc));
							return Void();
						}
					}
				}
			}

			if (req.wantsTrueBest) {
				ASSERT(!bestOption.present());
				auto& startIndex =
				    req.preferLowerUtilization ? self->lowestUtilizationTeam : self->highestUtilizationTeam;
				if (startIndex >= self->teams.size()) {
					startIndex = 0;
				}

				int bestIndex = startIndex;
				for (int i = 0; i < self->teams.size(); i++) {
					int currentIndex = (startIndex + i) % self->teams.size();
					if (self->teams[currentIndex]->isHealthy() &&
					    (!req.preferLowerUtilization ||
					     self->teams[currentIndex]->hasHealthyAvailableSpace(self->medianAvailableSpace))) {
						int64_t loadBytes = self->teams[currentIndex]->getLoadBytes(true, req.inflightPenalty);
						if ((!bestOption.present() || (req.preferLowerUtilization && loadBytes < bestLoadBytes) ||
						     (!req.preferLowerUtilization && loadBytes > bestLoadBytes)) &&
						    (!req.teamMustHaveShards ||
						     self->shardsAffectedByTeamFailure->hasShards(ShardsAffectedByTeamFailure::Team(
						         self->teams[currentIndex]->getServerIDs(), self->primary)))) {
							bestLoadBytes = loadBytes;
							bestOption = self->teams[currentIndex];
							bestIndex = currentIndex;
						}
					}
				}

				startIndex = bestIndex;
			} else {
				int nTries = 0;
				while (randomTeams.size() < SERVER_KNOBS->BEST_TEAM_OPTION_COUNT &&
				       nTries < SERVER_KNOBS->BEST_TEAM_MAX_TEAM_TRIES) {
					// If unhealthy team is majority, we may not find an ok dest in this while loop
					Reference<IDataDistributionTeam> dest = deterministicRandom()->randomChoice(self->teams);

					bool ok = dest->isHealthy() && (!req.preferLowerUtilization ||
					                                dest->hasHealthyAvailableSpace(self->medianAvailableSpace));

					for (int i = 0; ok && i < randomTeams.size(); i++) {
						if (randomTeams[i]->getServerIDs() == dest->getServerIDs()) {
							ok = false;
							break;
						}
					}

					ok = ok && (!req.teamMustHaveShards ||
					            self->shardsAffectedByTeamFailure->hasShards(
					                ShardsAffectedByTeamFailure::Team(dest->getServerIDs(), self->primary)));

					if (ok)
						randomTeams.push_back(dest);
					else
						nTries++;
				}

				// Log BestTeamStuck reason when we have healthy teams but they do not have healthy free space
				if (randomTeams.empty() && !self->zeroHealthyTeams->get()) {
					self->bestTeamKeepStuckCount++;
					if (g_network->isSimulated()) {
						TraceEvent(SevWarn, "GetTeamReturnEmpty").detail("HealthyTeams", self->healthyTeamCount);
					}
				} else {
					self->bestTeamKeepStuckCount = 0;
				}

				for (int i = 0; i < randomTeams.size(); i++) {
					int64_t loadBytes = randomTeams[i]->getLoadBytes(true, req.inflightPenalty);
					if (!bestOption.present() || (req.preferLowerUtilization && loadBytes < bestLoadBytes) ||
					    (!req.preferLowerUtilization && loadBytes > bestLoadBytes)) {
						bestLoadBytes = loadBytes;
						bestOption = randomTeams[i];
					}
				}
			}

			// Note: req.completeSources can be empty and all servers (and server teams) can be unhealthy.
			// We will get stuck at this! This only happens when a DC fails. No need to consider it right now.
			// Note: this block does not apply any filters from the request
			if (!bestOption.present() && self->zeroHealthyTeams->get()) {
				// Attempt to find the unhealthy source server team and return it
				for (int i = 0; i < req.completeSources.size(); i++) {
					if (!self->server_info.count(req.completeSources[i])) {
						continue;
					}
					auto& teamList = self->server_info[req.completeSources[i]]->teams;
					for (int j = 0; j < teamList.size(); j++) {
						bool found = true;
						auto serverIDs = teamList[j]->getServerIDs();
						for (int k = 0; k < teamList[j]->size(); k++) {
							if (!completeSources.count(serverIDs[k])) {
								found = false;
								break;
							}
						}
						if (found) {
							bestOption = teamList[j];
							req.reply.send(std::make_pair(bestOption, foundSrc));
							return Void();
						}
					}
				}
			}
			// if (!bestOption.present()) {
			// 	TraceEvent("GetTeamRequest").detail("Request", req.getDesc());
			// 	self->traceAllInfo(true);
			// }

			req.reply.send(std::make_pair(bestOption, foundSrc));

			return Void();
		} catch (Error& e) {
			if (e.code() != error_code_actor_cancelled)
				req.reply.sendError(e);
			throw;
		}
	}

	int64_t getDebugTotalDataInFlight() const {
		int64_t total = 0;
		for (auto itr = server_info.begin(); itr != server_info.end(); ++itr)
			total += itr->second->dataInFlightToServer;
		return total;
	}

	ACTOR static Future<Void> addSubsetOfEmergencyTeams(DDTeamCollection* self) {
		state int idx = 0;
		state std::vector<Reference<TCServerInfo>> servers;
		state std::vector<UID> serverIds;
		state Reference<LocalitySet> tempSet = Reference<LocalitySet>(new LocalityMap<UID>());
		state LocalityMap<UID>* tempMap = (LocalityMap<UID>*)tempSet.getPtr();

		for (; idx < self->badTeams.size(); idx++) {
			servers.clear();
			for (const auto& server : self->badTeams[idx]->getServers()) {
				if (server->inDesiredDC && !self->server_status.get(server->id).isUnhealthy()) {
					servers.push_back(server);
				}
			}

			// For the bad team that is too big (too many servers), we will try to find a subset of servers in the team
			// to construct a new healthy team, so that moving data to the new healthy team will not
			// cause too much data movement overhead
			// FIXME: This code logic can be simplified.
			if (servers.size() >= self->configuration.storageTeamSize) {
				bool foundTeam = false;
				for (int j = 0; j < servers.size() - self->configuration.storageTeamSize + 1 && !foundTeam; j++) {
					auto& serverTeams = servers[j]->teams;
					for (int k = 0; k < serverTeams.size(); k++) {
						auto& testTeam = serverTeams[k]->getServerIDs();
						bool allInTeam = true; // All servers in testTeam belong to the healthy servers
						for (int l = 0; l < testTeam.size(); l++) {
							bool foundServer = false;
							for (auto it : servers) {
								if (it->id == testTeam[l]) {
									foundServer = true;
									break;
								}
							}
							if (!foundServer) {
								allInTeam = false;
								break;
							}
						}
						if (allInTeam) {
							foundTeam = true;
							break;
						}
					}
				}
				if (!foundTeam) {
					if (self->satisfiesPolicy(servers)) {
						if (servers.size() == self->configuration.storageTeamSize ||
						    self->satisfiesPolicy(servers, self->configuration.storageTeamSize)) {
							servers.resize(self->configuration.storageTeamSize);
							self->addTeam(servers, true);
							// self->traceTeamCollectionInfo(); // Trace at the end of the function
						} else {
							tempSet->clear();
							for (auto it : servers) {
								tempMap->add(it->lastKnownInterface.locality, &it->id);
							}

							std::vector<LocalityEntry> resultEntries, forcedEntries;
							bool result = tempSet->selectReplicas(
							    self->configuration.storagePolicy, forcedEntries, resultEntries);
							ASSERT(result && resultEntries.size() == self->configuration.storageTeamSize);

							serverIds.clear();
							for (auto& it : resultEntries) {
								serverIds.push_back(*tempMap->getObject(it));
							}
							std::sort(serverIds.begin(), serverIds.end());
							self->addTeam(serverIds.begin(), serverIds.end(), true);
						}
					} else {
						serverIds.clear();
						for (auto it : servers) {
							serverIds.push_back(it->id);
						}
						TraceEvent(SevWarnAlways, "CannotAddSubset", self->distributorId)
						    .detail("Servers", describe(serverIds));
					}
				}
			}
			wait(yield());
		}

		// Trace and record the current number of teams for correctness test
		self->traceTeamCollectionInfo();

		return Void();
	}

	ACTOR static Future<Void> init(DDTeamCollection* self,
	                               Reference<InitialDataDistribution> initTeams,
	                               const DDEnabledState* ddEnabledState) {
		self->healthyZone.set(initTeams->initHealthyZoneValue);
		// SOMEDAY: If some servers have teams and not others (or some servers have more data than others) and there is
		// an address/locality collision, should we preferentially mark the least used server as undesirable?

		for (auto i = initTeams->allServers.begin(); i != initTeams->allServers.end(); ++i) {
			if (self->shouldHandleServer(i->first)) {
				if (!self->isValidLocality(self->configuration.storagePolicy, i->first.locality)) {
					TraceEvent(SevWarnAlways, "MissingLocality")
					    .detail("Server", i->first.uniqueID)
					    .detail("Locality", i->first.locality.toString());
					auto addr = i->first.stableAddress();
					self->invalidLocalityAddr.insert(AddressExclusion(addr.ip, addr.port));
					if (self->checkInvalidLocalities.isReady()) {
						self->checkInvalidLocalities = checkAndRemoveInvalidLocalityAddr(self);
						self->addActor.send(self->checkInvalidLocalities);
					}
				}
				self->addServer(i->first, i->second, self->serverTrackerErrorOut, 0, ddEnabledState);
			}
		}

		state std::set<std::vector<UID>>::iterator teamIter =
		    self->primary ? initTeams->primaryTeams.begin() : initTeams->remoteTeams.begin();
		state std::set<std::vector<UID>>::iterator teamIterEnd =
		    self->primary ? initTeams->primaryTeams.end() : initTeams->remoteTeams.end();
		for (; teamIter != teamIterEnd; ++teamIter) {
			self->addTeam(teamIter->begin(), teamIter->end(), true);
			wait(yield());
		}

		return Void();
	}

	// Check if server or machine has a valid locality based on configured replication policy
	bool isValidLocality(Reference<IReplicationPolicy> storagePolicy, const LocalityData& locality) const {
		// Future: Once we add simulation test that misconfigure a cluster, such as not setting some locality entries,
		// DD_VALIDATE_LOCALITY should always be true. Otherwise, simulation test may fail.
		if (!SERVER_KNOBS->DD_VALIDATE_LOCALITY) {
			// Disable the checking if locality is valid
			return true;
		}

		std::set<std::string> replicationPolicyKeys = storagePolicy->attributeKeys();
		for (auto& policy : replicationPolicyKeys) {
			if (!locality.isPresent(policy)) {
				return false;
			}
		}

		return true;
	}

	void evaluateTeamQuality() const {
		int teamCount = teams.size(), serverCount = allServers.size();
		double teamsPerServer = (double)teamCount * configuration.storageTeamSize / serverCount;

		ASSERT(serverCount == server_info.size());

		int minTeams = std::numeric_limits<int>::max();
		int maxTeams = std::numeric_limits<int>::min();
		double varTeams = 0;

		std::map<Optional<Standalone<StringRef>>, int> machineTeams;
		for (const auto& [id, info] : server_info) {
			if (!server_status.get(id).isUnhealthy()) {
				int stc = info->teams.size();
				minTeams = std::min(minTeams, stc);
				maxTeams = std::max(maxTeams, stc);
				varTeams += (stc - teamsPerServer) * (stc - teamsPerServer);
				// Use zoneId as server's machine id
				machineTeams[info->lastKnownInterface.locality.zoneId()] += stc;
			}
		}
		varTeams /= teamsPerServer * teamsPerServer;

		int minMachineTeams = std::numeric_limits<int>::max();
		int maxMachineTeams = std::numeric_limits<int>::min();
		for (auto m = machineTeams.begin(); m != machineTeams.end(); ++m) {
			minMachineTeams = std::min(minMachineTeams, m->second);
			maxMachineTeams = std::max(maxMachineTeams, m->second);
		}

		TraceEvent(minTeams > 0 ? SevInfo : SevWarn, "DataDistributionTeamQuality", distributorId)
		    .detail("Servers", serverCount)
		    .detail("Teams", teamCount)
		    .detail("TeamsPerServer", teamsPerServer)
		    .detail("Variance", varTeams / serverCount)
		    .detail("ServerMinTeams", minTeams)
		    .detail("ServerMaxTeams", maxTeams)
		    .detail("MachineMinTeams", minMachineTeams)
		    .detail("MachineMaxTeams", maxMachineTeams);
	}

	int overlappingMembers(const std::vector<UID>& team) const {
		if (team.empty()) {
			return 0;
		}

		int maxMatchingServers = 0;
		const UID& serverID = team[0];
		const auto it = server_info.find(serverID);
		ASSERT(it != server_info.end());
		const auto& usedTeams = it->second->teams;
		for (const auto& usedTeam : usedTeams) {
			auto used = usedTeam->getServerIDs();
			int teamIdx = 0;
			int usedIdx = 0;
			int matchingServers = 0;
			while (teamIdx < team.size() && usedIdx < used.size()) {
				if (team[teamIdx] == used[usedIdx]) {
					matchingServers++;
					teamIdx++;
					usedIdx++;
				} else if (team[teamIdx] < used[usedIdx]) {
					teamIdx++;
				} else {
					usedIdx++;
				}
			}
			ASSERT(matchingServers > 0);
			maxMatchingServers = std::max(maxMatchingServers, matchingServers);
			if (maxMatchingServers == team.size()) {
				return maxMatchingServers;
			}
		}

		return maxMatchingServers;
	}

	int overlappingMachineMembers(std::vector<Standalone<StringRef>> const& team) const {
		if (team.empty()) {
			return 0;
		}

		int maxMatchingServers = 0;
		auto it = machine_info.find(team[0]);
		ASSERT(it != machine_info.end());
		auto const& machineTeams = it->second->machineTeams;
		for (auto const& usedTeam : machineTeams) {
			auto used = usedTeam->machineIDs;
			int teamIdx = 0;
			int usedIdx = 0;
			int matchingServers = 0;
			while (teamIdx < team.size() && usedIdx < used.size()) {
				if (team[teamIdx] == used[usedIdx]) {
					matchingServers++;
					teamIdx++;
					usedIdx++;
				} else if (team[teamIdx] < used[usedIdx]) {
					teamIdx++;
				} else {
					usedIdx++;
				}
			}
			ASSERT(matchingServers > 0);
			maxMatchingServers = std::max(maxMatchingServers, matchingServers);
			if (maxMatchingServers == team.size()) {
				return maxMatchingServers;
			}
		}

		return maxMatchingServers;
	}

	Reference<TCMachineTeamInfo> findMachineTeam(std::vector<Standalone<StringRef>> const& machineIDs) const {
		if (machineIDs.empty()) {
			return Reference<TCMachineTeamInfo>();
		}

		Standalone<StringRef> machineID = machineIDs[0];
		for (auto& machineTeam : get(machine_info, machineID)->machineTeams) {
			if (machineTeam->machineIDs == machineIDs) {
				return machineTeam;
			}
		}

		return Reference<TCMachineTeamInfo>();
	}

	// Assume begin to end is sorted by std::sort
	// Assume InputIt is iterator to UID
	// Note: We must allow creating empty teams because empty team is created when a remote DB is initialized.
	// The empty team is used as the starting point to move data to the remote DB
	// begin : the start of the team member ID
	// end : end of the team member ID
	// isIntialTeam : False when the team is added by addTeamsBestOf(); True otherwise, e.g.,
	// when the team added at init() when we recreate teams by looking up DB
	template <class InputIt>
	void addTeam(InputIt begin, InputIt end, bool isInitialTeam) {
		std::vector<Reference<TCServerInfo>> newTeamServers;
		for (auto i = begin; i != end; ++i) {
			if (server_info.find(*i) != server_info.end()) {
				newTeamServers.push_back(server_info[*i]);
			}
		}

		addTeam(newTeamServers, isInitialTeam);
	}

	void addTeam(const std::vector<Reference<TCServerInfo>>& newTeamServers,
	             bool isInitialTeam,
	             bool redundantTeam = false) {
		auto teamInfo = makeReference<TCTeamInfo>(newTeamServers);

		// Move satisfiesPolicy to the end for performance benefit
		bool badTeam = redundantTeam || teamInfo->size() != configuration.storageTeamSize ||
		               !satisfiesPolicy(teamInfo->getServers());

		teamInfo->tracker = teamTracker(this, teamInfo, badTeam, redundantTeam);
		// ASSERT( teamInfo->serverIDs.size() > 0 ); //team can be empty at DB initialization
		if (badTeam) {
			badTeams.push_back(teamInfo);
			return;
		}

		// For a good team, we add it to teams and create machine team for it when necessary
		teams.push_back(teamInfo);
		for (int i = 0; i < newTeamServers.size(); ++i) {
			newTeamServers[i]->teams.push_back(teamInfo);
		}

		// Find or create machine team for the server team
		// Add the reference of machineTeam (with machineIDs) into process team
		std::vector<Standalone<StringRef>> machineIDs;
		for (auto server = newTeamServers.begin(); server != newTeamServers.end(); ++server) {
			ASSERT_WE_THINK((*server)->machine.isValid());
			machineIDs.push_back((*server)->machine->machineID);
		}
		sort(machineIDs.begin(), machineIDs.end());
		Reference<TCMachineTeamInfo> machineTeamInfo = findMachineTeam(machineIDs);

		// A team is not initial team if it is added by addTeamsBestOf() which always create a team with correct size
		// A non-initial team must have its machine team created and its size must be correct
		ASSERT(isInitialTeam || machineTeamInfo.isValid());

		// Create a machine team if it does not exist
		// Note an initial team may be added at init() even though the team size is not storageTeamSize
		if (!machineTeamInfo.isValid() && !machineIDs.empty()) {
			machineTeamInfo = addMachineTeam(machineIDs.begin(), machineIDs.end());
		}

		if (!machineTeamInfo.isValid()) {
			TraceEvent(SevWarn, "AddTeamWarning")
			    .detail("NotFoundMachineTeam", "OKIfTeamIsEmpty")
			    .detail("TeamInfo", teamInfo->getDesc());
		}

		teamInfo->machineTeam = machineTeamInfo;
		machineTeamInfo->serverTeams.push_back(teamInfo);
		if (g_network->isSimulated()) {
			// Update server team information for consistency check in simulation
			traceTeamCollectionInfo();
		}
	}

	void addTeam(std::set<UID> const& team, bool isInitialTeam) { addTeam(team.begin(), team.end(), isInitialTeam); }

	// Add a machine team specified by input machines
	Reference<TCMachineTeamInfo> addMachineTeam(std::vector<Reference<TCMachineInfo>> machines) {
		auto machineTeamInfo = makeReference<TCMachineTeamInfo>(machines);
		machineTeams.push_back(machineTeamInfo);

		// Assign machine teams to machine
		for (auto machine : machines) {
			// A machine's machineTeams vector should not hold duplicate machineTeam members
			ASSERT_WE_THINK(std::count(machine->machineTeams.begin(), machine->machineTeams.end(), machineTeamInfo) ==
			                0);
			machine->machineTeams.push_back(machineTeamInfo);
		}

		return machineTeamInfo;
	}

	// Add a machine team by using the machineIDs from begin to end
	Reference<TCMachineTeamInfo> addMachineTeam(std::vector<Standalone<StringRef>>::iterator begin,
	                                            std::vector<Standalone<StringRef>>::iterator end) {
		std::vector<Reference<TCMachineInfo>> machines;

		for (auto i = begin; i != end; ++i) {
			if (machine_info.find(*i) != machine_info.end()) {
				machines.push_back(machine_info[*i]);
			} else {
				TraceEvent(SevWarn, "AddMachineTeamError").detail("MachineIDNotExist", i->contents().toString());
			}
		}

		return addMachineTeam(machines);
	}

	// Group storage servers (process) based on their machineId in LocalityData
	// All created machines are healthy
	// Return The number of healthy servers we grouped into machines
	int constructMachinesFromServers() {
		int totalServerIndex = 0;
		for (auto i = server_info.begin(); i != server_info.end(); ++i) {
			if (!server_status.get(i->first).isUnhealthy()) {
				checkAndCreateMachine(i->second);
				totalServerIndex++;
			}
		}

		return totalServerIndex;
	}

	void traceConfigInfo() const {
		TraceEvent("DDConfig", distributorId)
		    .detail("StorageTeamSize", configuration.storageTeamSize)
		    .detail("DesiredTeamsPerServer", SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER)
		    .detail("MaxTeamsPerServer", SERVER_KNOBS->MAX_TEAMS_PER_SERVER)
		    .detail("StoreType", configuration.storageServerStoreType);
	}

	void traceServerInfo() const {
		int i = 0;

		TraceEvent("ServerInfo", distributorId).detail("Size", server_info.size());
		for (auto& server : server_info) {
			TraceEvent("ServerInfo", distributorId)
			    .detail("ServerInfoIndex", i++)
			    .detail("ServerID", server.first.toString())
			    .detail("ServerTeamOwned", server.second->teams.size())
			    .detail("MachineID", server.second->machine->machineID.contents().toString())
			    .detail("StoreType", server.second->storeType.toString())
			    .detail("InDesiredDC", server.second->inDesiredDC);
		}
		for (auto& server : server_info) {
			const UID& uid = server.first;
			TraceEvent("ServerStatus", distributorId)
			    .detail("ServerID", uid)
			    .detail("Healthy", !server_status.get(uid).isUnhealthy())
			    .detail("MachineIsValid", get(server_info, uid)->machine.isValid())
			    .detail("MachineTeamSize",
			            get(server_info, uid)->machine.isValid() ? get(server_info, uid)->machine->machineTeams.size()
			                                                     : -1);
		}
	}

	void traceServerTeamInfo() const {
		int i = 0;

		TraceEvent("ServerTeamInfo", distributorId).detail("Size", teams.size());
		for (auto& team : teams) {
			TraceEvent("ServerTeamInfo", distributorId)
			    .detail("TeamIndex", i++)
			    .detail("Healthy", team->isHealthy())
			    .detail("TeamSize", team->size())
			    .detail("MemberIDs", team->getServerIDsStr())
			    .detail("TeamID", team->getTeamID());
		}
	}

	void traceMachineInfo() const {
		int i = 0;

		TraceEvent("MachineInfo").detail("Size", machine_info.size());
		for (auto& machine : machine_info) {
			TraceEvent("MachineInfo", distributorId)
			    .detail("MachineInfoIndex", i++)
			    .detail("Healthy", isMachineHealthy(machine.second))
			    .detail("MachineID", machine.first.contents().toString())
			    .detail("MachineTeamOwned", machine.second->machineTeams.size())
			    .detail("ServerNumOnMachine", machine.second->serversOnMachine.size())
			    .detail("ServersID", machine.second->getServersIDStr());
		}
	}

	void traceMachineTeamInfo() const {
		int i = 0;

		TraceEvent("MachineTeamInfo", distributorId).detail("Size", machineTeams.size());
		for (auto& team : machineTeams) {
			TraceEvent("MachineTeamInfo", distributorId)
			    .detail("TeamIndex", i++)
			    .detail("MachineIDs", team->getMachineIDsStr())
			    .detail("ServerTeams", team->serverTeams.size());
		}
	}

	// Locality string is hashed into integer, used as KeyIndex
	// For better understand which KeyIndex is used for locality, we print this info in trace.
	void traceLocalityArrayIndexName() const {
		TraceEvent("LocalityRecordKeyName").detail("Size", machineLocalityMap._keymap->_lookuparray.size());
		for (int i = 0; i < machineLocalityMap._keymap->_lookuparray.size(); ++i) {
			TraceEvent("LocalityRecordKeyIndexName")
			    .detail("KeyIndex", i)
			    .detail("KeyName", machineLocalityMap._keymap->_lookuparray[i]);
		}
	}

	void traceMachineLocalityMap() const {
		int i = 0;

		TraceEvent("MachineLocalityMap", distributorId).detail("Size", machineLocalityMap.size());
		for (auto& uid : machineLocalityMap.getObjects()) {
			Reference<LocalityRecord> record = machineLocalityMap.getRecord(i);
			if (record.isValid()) {
				TraceEvent("MachineLocalityMap", distributorId)
				    .detail("LocalityIndex", i++)
				    .detail("UID", uid->toString())
				    .detail("LocalityRecord", record->toString());
			} else {
				TraceEvent("MachineLocalityMap")
				    .detail("LocalityIndex", i++)
				    .detail("UID", uid->toString())
				    .detail("LocalityRecord", "[NotFound]");
			}
		}
	}

	// To enable verbose debug info, set shouldPrint to true
	void traceAllInfo(bool shouldPrint = false) const {

		if (!shouldPrint)
			return;
		// Record all team collections IDs
		for (int i = 0; i < teamCollections.size(); ++i) {
			if (teamCollections[i] != nullptr) {
				TraceEvent("TraceAllInfo", distributorId)
				    .detail("TeamCollectionIndex", i)
				    .detail("Primary", teamCollections[i]->primary);
			}
		}

		TraceEvent("TraceAllInfo", distributorId).detail("Primary", primary);
		traceConfigInfo();
		traceServerInfo();
		traceServerTeamInfo();
		traceMachineInfo();
		traceMachineTeamInfo();
		traceLocalityArrayIndexName();
		traceMachineLocalityMap();
	}

	// We must rebuild machine locality map whenever the entry in the map is inserted or removed
	void rebuildMachineLocalityMap() {
		machineLocalityMap.clear();
		int numHealthyMachine = 0;
		for (auto machine = machine_info.begin(); machine != machine_info.end(); ++machine) {
			if (machine->second->serversOnMachine.empty()) {
				TraceEvent(SevWarn, "RebuildMachineLocalityMapError")
				    .detail("Machine", machine->second->machineID.toString())
				    .detail("NumServersOnMachine", 0);
				continue;
			}
			if (!isMachineHealthy(machine->second)) {
				continue;
			}
			Reference<TCServerInfo> representativeServer = machine->second->serversOnMachine[0];
			auto& locality = representativeServer->lastKnownInterface.locality;
			if (!isValidLocality(configuration.storagePolicy, locality)) {
				TraceEvent(SevWarn, "RebuildMachineLocalityMapError")
				    .detail("Machine", machine->second->machineID.toString())
				    .detail("InvalidLocality", locality.toString());
				continue;
			}
			const LocalityEntry& localityEntry = machineLocalityMap.add(locality, &representativeServer->id);
			machine->second->localityEntry = localityEntry;
			++numHealthyMachine;
		}
	}

	// Create machineTeamsToBuild number of machine teams
	// No operation if machineTeamsToBuild is 0
	// Note: The creation of machine teams should not depend on server teams:
	// No matter how server teams will be created, we will create the same set of machine teams;
	// We should never use server team number in building machine teams.
	//
	// Five steps to create each machine team, which are document in the function
	// Reuse ReplicationPolicy selectReplicas func to select machine team
	// return number of added machine teams
	int addBestMachineTeams(int machineTeamsToBuild) {
		int addedMachineTeams = 0;

		ASSERT(machineTeamsToBuild >= 0);
		// The number of machines is always no smaller than the storageTeamSize in a correct configuration
		ASSERT(machine_info.size() >= configuration.storageTeamSize);
		// Future: Consider if we should overbuild more machine teams to
		// allow machineTeamRemover() to get a more balanced machine teams per machine

		// Step 1: Create machineLocalityMap which will be used in building machine team
		rebuildMachineLocalityMap();

		// Add a team in each iteration
		while (addedMachineTeams < machineTeamsToBuild || notEnoughMachineTeamsForAMachine()) {
			// Step 2: Get least used machines from which we choose machines as a machine team
			std::vector<Reference<TCMachineInfo>> leastUsedMachines; // A less used machine has less number of teams
			int minTeamCount = std::numeric_limits<int>::max();
			for (auto& machine : machine_info) {
				// Skip invalid machine whose representative server is not in server_info
				ASSERT_WE_THINK(server_info.find(machine.second->serversOnMachine[0]->id) != server_info.end());
				// Skip unhealthy machines
				if (!isMachineHealthy(machine.second))
					continue;
				// Skip machine with incomplete locality
				if (!isValidLocality(configuration.storagePolicy,
				                     machine.second->serversOnMachine[0]->lastKnownInterface.locality)) {
					continue;
				}

				// Invariant: We only create correct size machine teams.
				// When configuration (e.g., team size) is changed, the DDTeamCollection will be destroyed and rebuilt
				// so that the invariant will not be violated.
				int teamCount = machine.second->machineTeams.size();

				if (teamCount < minTeamCount) {
					leastUsedMachines.clear();
					minTeamCount = teamCount;
				}
				if (teamCount == minTeamCount) {
					leastUsedMachines.push_back(machine.second);
				}
			}

			std::vector<UID*> team;
			std::vector<LocalityEntry> forcedAttributes;

			// Step 4: Reuse Policy's selectReplicas() to create team for the representative process.
			std::vector<UID*> bestTeam;
			int bestScore = std::numeric_limits<int>::max();
			int maxAttempts = SERVER_KNOBS->BEST_OF_AMT; // BEST_OF_AMT = 4
			for (int i = 0; i < maxAttempts && i < 100; ++i) {
				// Step 3: Create a representative process for each machine.
				// Construct forcedAttribute from leastUsedMachines.
				// We will use forcedAttribute to call existing function to form a team
				if (leastUsedMachines.size()) {
					forcedAttributes.clear();
					// Randomly choose 1 least used machine
					Reference<TCMachineInfo> tcMachineInfo = deterministicRandom()->randomChoice(leastUsedMachines);
					ASSERT(!tcMachineInfo->serversOnMachine.empty());
					LocalityEntry process = tcMachineInfo->localityEntry;
					forcedAttributes.push_back(process);
					TraceEvent("ChosenMachine")
					    .detail("MachineInfo", tcMachineInfo->machineID)
					    .detail("LeaseUsedMachinesSize", leastUsedMachines.size())
					    .detail("ForcedAttributesSize", forcedAttributes.size());
				} else {
					// when leastUsedMachine is empty, we will never find a team later, so we can simply return.
					return addedMachineTeams;
				}

				// Choose a team that balances the # of teams per server among the teams
				// that have the least-utilized server
				team.clear();
				ASSERT_WE_THINK(forcedAttributes.size() == 1);
				auto success = machineLocalityMap.selectReplicas(configuration.storagePolicy, forcedAttributes, team);
				// NOTE: selectReplicas() should always return success when storageTeamSize = 1
				ASSERT_WE_THINK(configuration.storageTeamSize > 1 || (configuration.storageTeamSize == 1 && success));
				if (!success) {
					continue; // Try up to maxAttempts, since next time we may choose a different forcedAttributes
				}
				ASSERT(forcedAttributes.size() > 0);
				team.push_back((UID*)machineLocalityMap.getObject(forcedAttributes[0]));

				// selectReplicas() may NEVER return server not in server_info.
				for (auto& pUID : team) {
					ASSERT_WE_THINK(server_info.find(*pUID) != server_info.end());
				}

				// selectReplicas() should always return a team with correct size. otherwise, it has a bug
				ASSERT(team.size() == configuration.storageTeamSize);

				int score = 0;
				std::vector<Standalone<StringRef>> machineIDs;
				for (auto process = team.begin(); process != team.end(); process++) {
					Reference<TCServerInfo> server = server_info[**process];
					score += server->machine->machineTeams.size();
					Standalone<StringRef> machine_id = server->lastKnownInterface.locality.zoneId().get();
					machineIDs.push_back(machine_id);
				}

				// Only choose healthy machines into machine team
				ASSERT_WE_THINK(isMachineTeamHealthy(machineIDs));

				std::sort(machineIDs.begin(), machineIDs.end());
				int overlap = overlappingMachineMembers(machineIDs);
				if (overlap == machineIDs.size()) {
					maxAttempts += 1;
					continue;
				}
				score += SERVER_KNOBS->DD_OVERLAP_PENALTY * overlap;

				// SOMEDAY: randomly pick one from teams with the lowest score
				if (score < bestScore) {
					// bestTeam is the team which has the smallest number of teams its team members belong to.
					bestTeam = team;
					bestScore = score;
				}
			}

			// bestTeam should be a new valid team to be added into machine team now
			// Step 5: Restore machine from its representative process team and get the machine team
			if (bestTeam.size() == configuration.storageTeamSize) {
				// machineIDs is used to quickly check if the machineIDs belong to an existed team
				// machines keep machines reference for performance benefit by avoiding looking up machine by machineID
				std::vector<Reference<TCMachineInfo>> machines;
				for (auto process = bestTeam.begin(); process < bestTeam.end(); process++) {
					Reference<TCMachineInfo> machine = server_info[**process]->machine;
					machines.push_back(machine);
				}

				addMachineTeam(machines);
				addedMachineTeams++;
			} else {
				traceAllInfo(true);
				TraceEvent(SevWarn, "DataDistributionBuildTeams", distributorId)
				    .detail("Primary", primary)
				    .detail("Reason", "Unable to make desired machine Teams");
				lastBuildTeamsFailed = true;
				break;
			}
		}

		return addedMachineTeams;
	}

	bool isMachineTeamHealthy(std::vector<Standalone<StringRef>> const& machineIDs) const {
		int healthyNum = 0;

		// A healthy machine team should have the desired number of machines
		if (machineIDs.size() != configuration.storageTeamSize)
			return false;

		for (auto& id : machineIDs) {
			auto& machine = get(machine_info, id);
			if (isMachineHealthy(machine)) {
				healthyNum++;
			}
		}
		return (healthyNum == machineIDs.size());
	}

	bool isMachineTeamHealthy(TCMachineTeamInfo const& machineTeam) const {
		int healthyNum = 0;

		// A healthy machine team should have the desired number of machines
		if (machineTeam.size() != configuration.storageTeamSize)
			return false;

		for (auto& machine : machineTeam.machines) {
			if (isMachineHealthy(machine)) {
				healthyNum++;
			}
		}
		return (healthyNum == machineTeam.machines.size());
	}

	bool isMachineHealthy(Reference<TCMachineInfo> const& machine) const {
		if (!machine.isValid() || machine_info.find(machine->machineID) == machine_info.end() ||
		    machine->serversOnMachine.empty()) {
			return false;
		}

		// Healthy machine has at least one healthy server
		for (auto& server : machine->serversOnMachine) {
			if (!server_status.get(server->id).isUnhealthy()) {
				return true;
			}
		}

		return false;
	}

	// Return the healthy server with the least number of correct-size server teams
	Reference<TCServerInfo> findOneLeastUsedServer() const {
		std::vector<Reference<TCServerInfo>> leastUsedServers;
		int minTeams = std::numeric_limits<int>::max();
		for (auto& server : server_info) {
			// Only pick healthy server, which is not failed or excluded.
			if (server_status.get(server.first).isUnhealthy())
				continue;
			if (!isValidLocality(configuration.storagePolicy, server.second->lastKnownInterface.locality))
				continue;

			int numTeams = server.second->teams.size();
			if (numTeams < minTeams) {
				minTeams = numTeams;
				leastUsedServers.clear();
			}
			if (minTeams == numTeams) {
				leastUsedServers.push_back(server.second);
			}
		}

		if (leastUsedServers.empty()) {
			// If we cannot find a healthy server with valid locality
			TraceEvent("NoHealthyAndValidLocalityServers")
			    .detail("Servers", server_info.size())
			    .detail("UnhealthyServers", unhealthyServers);
			return Reference<TCServerInfo>();
		} else {
			return deterministicRandom()->randomChoice(leastUsedServers);
		}
	}

	// Randomly choose one machine team that has chosenServer and has the correct size
	// When configuration is changed, we may have machine teams with old storageTeamSize
	Reference<TCMachineTeamInfo> findOneRandomMachineTeam(TCServerInfo const& chosenServer) const {
		if (!chosenServer.machine->machineTeams.empty()) {
			std::vector<Reference<TCMachineTeamInfo>> healthyMachineTeamsForChosenServer;
			for (auto& mt : chosenServer.machine->machineTeams) {
				if (isMachineTeamHealthy(*mt)) {
					healthyMachineTeamsForChosenServer.push_back(mt);
				}
			}
			if (!healthyMachineTeamsForChosenServer.empty()) {
				return deterministicRandom()->randomChoice(healthyMachineTeamsForChosenServer);
			}
		}

		// If we cannot find a healthy machine team
		TraceEvent("NoHealthyMachineTeamForServer")
		    .detail("ServerID", chosenServer.id)
		    .detail("MachineTeams", chosenServer.machine->machineTeams.size());
		return Reference<TCMachineTeamInfo>();
	}

	// A server team should always come from servers on a machine team
	// Check if it is true
	bool isOnSameMachineTeam(TCTeamInfo const& team) const {
		std::vector<Standalone<StringRef>> machineIDs;
		for (const auto& server : team.getServers()) {
			if (!server->machine.isValid())
				return false;
			machineIDs.push_back(server->machine->machineID);
		}
		std::sort(machineIDs.begin(), machineIDs.end());

		int numExistance = 0;
		for (const auto& server : team.getServers()) {
			for (const auto& candidateMachineTeam : server->machine->machineTeams) {
				std::sort(candidateMachineTeam->machineIDs.begin(), candidateMachineTeam->machineIDs.end());
				if (machineIDs == candidateMachineTeam->machineIDs) {
					numExistance++;
					break;
				}
			}
		}
		return (numExistance == team.size());
	}

	// Sanity check the property of teams in unit test
	// Return true if all server teams belong to machine teams
	bool sanityCheckTeams() const {
		for (auto& team : teams) {
			if (isOnSameMachineTeam(*team) == false) {
				return false;
			}
		}

		return true;
	}

	int calculateHealthyServerCount() const {
		int serverCount = 0;
		for (auto i = server_info.begin(); i != server_info.end(); ++i) {
			if (!server_status.get(i->first).isUnhealthy()) {
				++serverCount;
			}
		}
		return serverCount;
	}

	int calculateHealthyMachineCount() const {
		int totalHealthyMachineCount = 0;
		for (auto& m : machine_info) {
			if (isMachineHealthy(m.second)) {
				++totalHealthyMachineCount;
			}
		}

		return totalHealthyMachineCount;
	}

	std::pair<int64_t, int64_t> calculateMinMaxServerTeamsOnServer() const {
		int64_t minTeams = std::numeric_limits<int64_t>::max();
		int64_t maxTeams = 0;
		for (auto& server : server_info) {
			if (server_status.get(server.first).isUnhealthy()) {
				continue;
			}
			minTeams = std::min((int64_t)server.second->teams.size(), minTeams);
			maxTeams = std::max((int64_t)server.second->teams.size(), maxTeams);
		}
		return std::make_pair(minTeams, maxTeams);
	}

	std::pair<int64_t, int64_t> calculateMinMaxMachineTeamsOnMachine() const {
		int64_t minTeams = std::numeric_limits<int64_t>::max();
		int64_t maxTeams = 0;
		for (auto& machine : machine_info) {
			if (!isMachineHealthy(machine.second)) {
				continue;
			}
			minTeams = std::min<int64_t>((int64_t)machine.second->machineTeams.size(), minTeams);
			maxTeams = std::max<int64_t>((int64_t)machine.second->machineTeams.size(), maxTeams);
		}
		return std::make_pair(minTeams, maxTeams);
	}

	// Sanity check
	bool isServerTeamCountCorrect(Reference<TCMachineTeamInfo> const& mt) const {
		int num = 0;
		bool ret = true;
		for (auto& team : teams) {
			if (team->machineTeam->machineIDs == mt->machineIDs) {
				++num;
			}
		}
		if (num != mt->serverTeams.size()) {
			ret = false;
			TraceEvent(SevError, "ServerTeamCountOnMachineIncorrect")
			    .detail("MachineTeam", mt->getMachineIDsStr())
			    .detail("ServerTeamsSize", mt->serverTeams.size())
			    .detail("CountedServerTeams", num);
		}
		return ret;
	}

	// Find the machine team with the least number of server teams
	std::pair<Reference<TCMachineTeamInfo>, int> getMachineTeamWithLeastProcessTeams() const {
		Reference<TCMachineTeamInfo> retMT;
		int minNumProcessTeams = std::numeric_limits<int>::max();

		for (auto& mt : machineTeams) {
			if (EXPENSIVE_VALIDATION) {
				ASSERT(isServerTeamCountCorrect(mt));
			}

			if (mt->serverTeams.size() < minNumProcessTeams) {
				minNumProcessTeams = mt->serverTeams.size();
				retMT = mt;
			}
		}

		return std::pair<Reference<TCMachineTeamInfo>, int>(retMT, minNumProcessTeams);
	}

	// Find the machine team whose members are on the most number of machine teams, same logic as serverTeamRemover
	std::pair<Reference<TCMachineTeamInfo>, int> getMachineTeamWithMostMachineTeams() const {
		Reference<TCMachineTeamInfo> retMT;
		int maxNumMachineTeams = 0;
		int targetMachineTeamNumPerMachine =
		    (SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * (configuration.storageTeamSize + 1)) / 2;

		for (auto& mt : machineTeams) {
			// The representative team number for the machine team mt is
			// the minimum number of machine teams of a machine in the team mt
			int representNumMachineTeams = std::numeric_limits<int>::max();
			for (auto& m : mt->machines) {
				representNumMachineTeams = std::min<int>(representNumMachineTeams, m->machineTeams.size());
			}
			if (representNumMachineTeams > targetMachineTeamNumPerMachine &&
			    representNumMachineTeams > maxNumMachineTeams) {
				maxNumMachineTeams = representNumMachineTeams;
				retMT = mt;
			}
		}

		return std::pair<Reference<TCMachineTeamInfo>, int>(retMT, maxNumMachineTeams);
	}

	// Find the server team whose members are on the most number of server teams
	std::pair<Reference<TCTeamInfo>, int> getServerTeamWithMostProcessTeams() const {
		Reference<TCTeamInfo> retST;
		int maxNumProcessTeams = 0;
		int targetTeamNumPerServer = (SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * (configuration.storageTeamSize + 1)) / 2;

		for (auto& t : teams) {
			// The minimum number of teams of a server in a team is the representative team number for the team t
			int representNumProcessTeams = std::numeric_limits<int>::max();
			for (auto& server : t->getServers()) {
				representNumProcessTeams = std::min<int>(representNumProcessTeams, server->teams.size());
			}
			// We only remove the team whose representNumProcessTeams is larger than the targetTeamNumPerServer number
			// otherwise, teamBuilder will build the to-be-removed team again
			if (representNumProcessTeams > targetTeamNumPerServer && representNumProcessTeams > maxNumProcessTeams) {
				maxNumProcessTeams = representNumProcessTeams;
				retST = t;
			}
		}

		return std::pair<Reference<TCTeamInfo>, int>(retST, maxNumProcessTeams);
	}

	int getHealthyMachineTeamCount() const {
		int healthyTeamCount = 0;
		for (const auto& mt : machineTeams) {
			ASSERT(mt->machines.size() == configuration.storageTeamSize);

			if (isMachineTeamHealthy(*mt)) {
				++healthyTeamCount;
			}
		}

		return healthyTeamCount;
	}

	// Each machine is expected to have targetMachineTeamNumPerMachine
	// Return true if there exists a machine that does not have enough teams.
	bool notEnoughMachineTeamsForAMachine() const {
		// If we want to remove the machine team with most machine teams, we use the same logic as
		// notEnoughTeamsForAServer
		int targetMachineTeamNumPerMachine =
		    SERVER_KNOBS->TR_FLAG_REMOVE_MT_WITH_MOST_TEAMS
		        ? (SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * (configuration.storageTeamSize + 1)) / 2
		        : SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER;
		for (auto& m : machine_info) {
			// If SERVER_KNOBS->TR_FLAG_REMOVE_MT_WITH_MOST_TEAMS is false,
			// The desired machine team number is not the same with the desired server team number
			// in notEnoughTeamsForAServer() below, because the machineTeamRemover() does not
			// remove a machine team with the most number of machine teams.
			if (m.second->machineTeams.size() < targetMachineTeamNumPerMachine && isMachineHealthy(m.second)) {
				return true;
			}
		}

		return false;
	}

	// Each server is expected to have targetTeamNumPerServer teams.
	// Return true if there exists a server that does not have enough teams.
	bool notEnoughTeamsForAServer() const {
		// We build more teams than we finally want so that we can use serverTeamRemover() actor to remove the teams
		// whose member belong to too many teams. This allows us to get a more balanced number of teams per server.
		// We want to ensure every server has targetTeamNumPerServer teams.
		// The numTeamsPerServerFactor is calculated as
		// (SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER + ideal_num_of_teams_per_server) / 2
		// ideal_num_of_teams_per_server is (#teams * storageTeamSize) / #servers, which is
		// (#servers * DESIRED_TEAMS_PER_SERVER * storageTeamSize) / #servers.
		int targetTeamNumPerServer = (SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * (configuration.storageTeamSize + 1)) / 2;
		ASSERT(targetTeamNumPerServer > 0);
		for (auto& s : server_info) {
			if (s.second->teams.size() < targetTeamNumPerServer && !server_status.get(s.first).isUnhealthy()) {
				return true;
			}
		}

		return false;
	}

	// Create server teams based on machine teams
	// Before the number of machine teams reaches the threshold, build a machine team for each server team
	// When it reaches the threshold, first try to build a server team with existing machine teams; if failed,
	// build an extra machine team and record the event in trace
	int addTeamsBestOf(int teamsToBuild, int desiredTeams, int maxTeams) {
		ASSERT(teamsToBuild >= 0);
		ASSERT_WE_THINK(machine_info.size() > 0 || server_info.size() == 0);
		ASSERT_WE_THINK(SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER >= 1 && configuration.storageTeamSize >= 1);

		int addedMachineTeams = 0;
		int addedTeams = 0;

		// Exclude machine teams who have members in the wrong configuration.
		// When we change configuration, we may have machine teams with storageTeamSize in the old configuration.
		int healthyMachineTeamCount = getHealthyMachineTeamCount();
		int totalMachineTeamCount = machineTeams.size();
		int totalHealthyMachineCount = calculateHealthyMachineCount();

		int desiredMachineTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * totalHealthyMachineCount;
		int maxMachineTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * totalHealthyMachineCount;
		// machineTeamsToBuild mimics how the teamsToBuild is calculated in buildTeams()
		int machineTeamsToBuild = std::max(
		    0, std::min(desiredMachineTeams - healthyMachineTeamCount, maxMachineTeams - totalMachineTeamCount));

		TraceEvent("BuildMachineTeams")
		    .detail("TotalHealthyMachine", totalHealthyMachineCount)
		    .detail("HealthyMachineTeamCount", healthyMachineTeamCount)
		    .detail("DesiredMachineTeams", desiredMachineTeams)
		    .detail("MaxMachineTeams", maxMachineTeams)
		    .detail("MachineTeamsToBuild", machineTeamsToBuild);
		// Pre-build all machine teams until we have the desired number of machine teams
		if (machineTeamsToBuild > 0 || notEnoughMachineTeamsForAMachine()) {
			addedMachineTeams = addBestMachineTeams(machineTeamsToBuild);
		}

		while (addedTeams < teamsToBuild || notEnoughTeamsForAServer()) {
			// Step 1: Create 1 best machine team
			std::vector<UID> bestServerTeam;
			int bestScore = std::numeric_limits<int>::max();
			int maxAttempts = SERVER_KNOBS->BEST_OF_AMT; // BEST_OF_AMT = 4
			bool earlyQuitBuild = false;
			for (int i = 0; i < maxAttempts && i < 100; ++i) {
				// Step 2: Choose 1 least used server and then choose 1 least used machine team from the server
				Reference<TCServerInfo> chosenServer = findOneLeastUsedServer();
				if (!chosenServer.isValid()) {
					TraceEvent(SevWarn, "NoValidServer").detail("Primary", primary);
					earlyQuitBuild = true;
					break;
				}
				// Note: To avoid creating correlation of picked machine teams, we simply choose a random machine team
				// instead of choosing the least used machine team.
				// The correlation happens, for example, when we add two new machines, we may always choose the machine
				// team with these two new machines because they are typically less used.
				Reference<TCMachineTeamInfo> chosenMachineTeam = findOneRandomMachineTeam(*chosenServer);

				if (!chosenMachineTeam.isValid()) {
					// We may face the situation that temporarily we have no healthy machine.
					TraceEvent(SevWarn, "MachineTeamNotFound")
					    .detail("Primary", primary)
					    .detail("MachineTeams", machineTeams.size());
					continue; // try randomly to find another least used server
				}

				// From here, chosenMachineTeam must have a healthy server team
				// Step 3: Randomly pick 1 server from each machine in the chosen machine team to form a server team
				std::vector<UID> serverTeam;
				int chosenServerCount = 0;
				for (auto& machine : chosenMachineTeam->machines) {
					UID serverID;
					if (machine == chosenServer->machine) {
						serverID = chosenServer->id;
						++chosenServerCount;
					} else {
						std::vector<Reference<TCServerInfo>> healthyProcesses;
						for (auto it : machine->serversOnMachine) {
							if (!server_status.get(it->id).isUnhealthy()) {
								healthyProcesses.push_back(it);
							}
						}
						serverID = deterministicRandom()->randomChoice(healthyProcesses)->id;
					}
					serverTeam.push_back(serverID);
				}

				ASSERT(chosenServerCount == 1); // chosenServer should be used exactly once
				ASSERT(serverTeam.size() == configuration.storageTeamSize);

				std::sort(serverTeam.begin(), serverTeam.end());
				int overlap = overlappingMembers(serverTeam);
				if (overlap == serverTeam.size()) {
					maxAttempts += 1;
					continue;
				}

				// Pick the server team with smallest score in all attempts
				// If we use different metric here, DD may oscillate infinitely in creating and removing teams.
				// SOMEDAY: Improve the code efficiency by using reservoir algorithm
				int score = SERVER_KNOBS->DD_OVERLAP_PENALTY * overlap;
				for (auto& server : serverTeam) {
					score += server_info[server]->teams.size();
				}
				TraceEvent(SevDebug, "BuildServerTeams")
				    .detail("Score", score)
				    .detail("BestScore", bestScore)
				    .detail("TeamSize", serverTeam.size())
				    .detail("StorageTeamSize", configuration.storageTeamSize);
				if (score < bestScore) {
					bestScore = score;
					bestServerTeam = serverTeam;
				}
			}

			if (earlyQuitBuild) {
				break;
			}
			if (bestServerTeam.size() != configuration.storageTeamSize) {
				// Not find any team and will unlikely find a team
				lastBuildTeamsFailed = true;
				break;
			}

			// Step 4: Add the server team
			addTeam(bestServerTeam.begin(), bestServerTeam.end(), false);
			addedTeams++;
		}

		healthyMachineTeamCount = getHealthyMachineTeamCount();

		std::pair<uint64_t, uint64_t> minMaxTeamsOnServer = calculateMinMaxServerTeamsOnServer();
		std::pair<uint64_t, uint64_t> minMaxMachineTeamsOnMachine = calculateMinMaxMachineTeamsOnMachine();

		TraceEvent("TeamCollectionInfo", distributorId)
		    .detail("Primary", primary)
		    .detail("AddedTeams", addedTeams)
		    .detail("TeamsToBuild", teamsToBuild)
		    .detail("CurrentServerTeams", teams.size())
		    .detail("DesiredTeams", desiredTeams)
		    .detail("MaxTeams", maxTeams)
		    .detail("StorageTeamSize", configuration.storageTeamSize)
		    .detail("CurrentMachineTeams", machineTeams.size())
		    .detail("CurrentHealthyMachineTeams", healthyMachineTeamCount)
		    .detail("DesiredMachineTeams", desiredMachineTeams)
		    .detail("MaxMachineTeams", maxMachineTeams)
		    .detail("TotalHealthyMachines", totalHealthyMachineCount)
		    .detail("MinTeamsOnServer", minMaxTeamsOnServer.first)
		    .detail("MaxTeamsOnServer", minMaxTeamsOnServer.second)
		    .detail("MinMachineTeamsOnMachine", minMaxMachineTeamsOnMachine.first)
		    .detail("MaxMachineTeamsOnMachine", minMaxMachineTeamsOnMachine.second)
		    .detail("DoBuildTeams", doBuildTeams)
		    .trackLatest("TeamCollectionInfo");

		return addedTeams;
	}

	// Check if the number of server (and machine teams) is larger than the maximum allowed number
	void traceTeamCollectionInfo() const {
		int totalHealthyServerCount = calculateHealthyServerCount();
		int desiredServerTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * totalHealthyServerCount;
		int maxServerTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * totalHealthyServerCount;

		int totalHealthyMachineCount = calculateHealthyMachineCount();
		int desiredMachineTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * totalHealthyMachineCount;
		int maxMachineTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * totalHealthyMachineCount;
		int healthyMachineTeamCount = getHealthyMachineTeamCount();

		std::pair<uint64_t, uint64_t> minMaxTeamsOnServer = calculateMinMaxServerTeamsOnServer();
		std::pair<uint64_t, uint64_t> minMaxMachineTeamsOnMachine = calculateMinMaxMachineTeamsOnMachine();

		TraceEvent("TeamCollectionInfo", distributorId)
		    .detail("Primary", primary)
		    .detail("AddedTeams", 0)
		    .detail("TeamsToBuild", 0)
		    .detail("CurrentServerTeams", teams.size())
		    .detail("DesiredTeams", desiredServerTeams)
		    .detail("MaxTeams", maxServerTeams)
		    .detail("StorageTeamSize", configuration.storageTeamSize)
		    .detail("CurrentMachineTeams", machineTeams.size())
		    .detail("CurrentHealthyMachineTeams", healthyMachineTeamCount)
		    .detail("DesiredMachineTeams", desiredMachineTeams)
		    .detail("MaxMachineTeams", maxMachineTeams)
		    .detail("TotalHealthyMachines", totalHealthyMachineCount)
		    .detail("MinTeamsOnServer", minMaxTeamsOnServer.first)
		    .detail("MaxTeamsOnServer", minMaxTeamsOnServer.second)
		    .detail("MinMachineTeamsOnMachine", minMaxMachineTeamsOnMachine.first)
		    .detail("MaxMachineTeamsOnMachine", minMaxMachineTeamsOnMachine.second)
		    .detail("DoBuildTeams", doBuildTeams)
		    .trackLatest("TeamCollectionInfo");

		// Advance time so that we will not have multiple TeamCollectionInfo at the same time, otherwise
		// simulation test will randomly pick one TeamCollectionInfo trace, which could be the one before build teams
		// wait(delay(0.01));

		// Debug purpose
		// if (healthyMachineTeamCount > desiredMachineTeams || machineTeams.size() > maxMachineTeams) {
		// 	// When the number of machine teams is over the limit, print out the current team info.
		// 	traceAllInfo(true);
		// }
	}

	// Use the current set of known processes (from server_info) to compute an optimized set of storage server teams.
	// The following are guarantees of the process:
	//   - Each newly-built team will meet the replication policy
	//   - All newly-built teams will have exactly teamSize machines
	//
	// buildTeams() only ever adds teams to the list of teams. Teams are only removed from the list when all data has
	// been removed.
	//
	// buildTeams will not count teams larger than teamSize against the desired teams.
	ACTOR static Future<Void> buildTeams(DDTeamCollection* self) {
		state int desiredTeams;
		int serverCount = 0;
		int uniqueMachines = 0;
		std::set<Optional<Standalone<StringRef>>> machines;

		for (auto i = self->server_info.begin(); i != self->server_info.end(); ++i) {
			if (!self->server_status.get(i->first).isUnhealthy()) {
				++serverCount;
				LocalityData& serverLocation = i->second->lastKnownInterface.locality;
				machines.insert(serverLocation.zoneId());
			}
		}
		uniqueMachines = machines.size();
		TraceEvent("BuildTeams", self->distributorId)
		    .detail("ServerCount", self->server_info.size())
		    .detail("UniqueMachines", uniqueMachines)
		    .detail("Primary", self->primary)
		    .detail("StorageTeamSize", self->configuration.storageTeamSize);

		// If there are too few machines to even build teams or there are too few represented datacenters, build no new
		// teams
		if (uniqueMachines >= self->configuration.storageTeamSize) {
			desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * serverCount;
			int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * serverCount;

			// Exclude teams who have members in the wrong configuration, since we don't want these teams
			int teamCount = 0;
			int totalTeamCount = 0;
			for (int i = 0; i < self->teams.size(); ++i) {
				if (!self->teams[i]->isWrongConfiguration()) {
					if (self->teams[i]->isHealthy()) {
						teamCount++;
					}
					totalTeamCount++;
				}
			}

			// teamsToBuild is calculated such that we will not build too many teams in the situation
			// when all (or most of) teams become unhealthy temporarily and then healthy again
			state int teamsToBuild = std::max(0, std::min(desiredTeams - teamCount, maxTeams - totalTeamCount));

			TraceEvent("BuildTeamsBegin", self->distributorId)
			    .detail("TeamsToBuild", teamsToBuild)
			    .detail("DesiredTeams", desiredTeams)
			    .detail("MaxTeams", maxTeams)
			    .detail("BadServerTeams", self->badTeams.size())
			    .detail("UniqueMachines", uniqueMachines)
			    .detail("TeamSize", self->configuration.storageTeamSize)
			    .detail("Servers", serverCount)
			    .detail("CurrentTrackedServerTeams", self->teams.size())
			    .detail("HealthyTeamCount", teamCount)
			    .detail("TotalTeamCount", totalTeamCount)
			    .detail("MachineTeamCount", self->machineTeams.size())
			    .detail("MachineCount", self->machine_info.size())
			    .detail("DesiredTeamsPerServer", SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER);

			self->lastBuildTeamsFailed = false;
			if (teamsToBuild > 0 || self->notEnoughTeamsForAServer()) {
				state std::vector<std::vector<UID>> builtTeams;

				// addTeamsBestOf() will not add more teams than needed.
				// If the team number is more than the desired, the extra teams are added in the code path when
				// a team is added as an initial team
				int addedTeams = self->addTeamsBestOf(teamsToBuild, desiredTeams, maxTeams);

				if (addedTeams <= 0 && self->teams.size() == 0) {
					TraceEvent(SevWarn, "NoTeamAfterBuildTeam", self->distributorId)
					    .detail("ServerTeamNum", self->teams.size())
					    .detail("Debug", "Check information below");
					// Debug: set true for traceAllInfo() to print out more information
					self->traceAllInfo();
				}
			} else {
				int totalHealthyMachineCount = self->calculateHealthyMachineCount();

				int desiredMachineTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * totalHealthyMachineCount;
				int maxMachineTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * totalHealthyMachineCount;
				int healthyMachineTeamCount = self->getHealthyMachineTeamCount();

				std::pair<uint64_t, uint64_t> minMaxTeamsOnServer = self->calculateMinMaxServerTeamsOnServer();
				std::pair<uint64_t, uint64_t> minMaxMachineTeamsOnMachine =
				    self->calculateMinMaxMachineTeamsOnMachine();

				TraceEvent("TeamCollectionInfo", self->distributorId)
				    .detail("Primary", self->primary)
				    .detail("AddedTeams", 0)
				    .detail("TeamsToBuild", teamsToBuild)
				    .detail("CurrentServerTeams", self->teams.size())
				    .detail("DesiredTeams", desiredTeams)
				    .detail("MaxTeams", maxTeams)
				    .detail("StorageTeamSize", self->configuration.storageTeamSize)
				    .detail("CurrentMachineTeams", self->machineTeams.size())
				    .detail("CurrentHealthyMachineTeams", healthyMachineTeamCount)
				    .detail("DesiredMachineTeams", desiredMachineTeams)
				    .detail("MaxMachineTeams", maxMachineTeams)
				    .detail("TotalHealthyMachines", totalHealthyMachineCount)
				    .detail("MinTeamsOnServer", minMaxTeamsOnServer.first)
				    .detail("MaxTeamsOnServer", minMaxTeamsOnServer.second)
				    .detail("MinMachineTeamsOnMachine", minMaxMachineTeamsOnMachine.first)
				    .detail("MaxMachineTeamsOnMachine", minMaxMachineTeamsOnMachine.second)
				    .detail("DoBuildTeams", self->doBuildTeams)
				    .trackLatest("TeamCollectionInfo");
			}
		} else {
			self->lastBuildTeamsFailed = true;
		}

		self->evaluateTeamQuality();

		// Building teams can cause servers to become undesired, which can make teams unhealthy.
		// Let all of these changes get worked out before responding to the get team request
		wait(delay(0, TaskPriority::DataDistributionLaunch));

		return Void();
	}

	void noHealthyTeams() const {
		std::set<UID> desiredServerSet;
		std::string desc;
		for (auto i = server_info.begin(); i != server_info.end(); ++i) {
			ASSERT(i->first == i->second->id);
			if (!server_status.get(i->first).isFailed) {
				desiredServerSet.insert(i->first);
				desc += i->first.shortString() + " (" + i->second->lastKnownInterface.toString() + "), ";
			}
		}

		TraceEvent(SevWarn, "NoHealthyTeams", distributorId)
		    .detail("CurrentServerTeamCount", teams.size())
		    .detail("ServerCount", server_info.size())
		    .detail("NonFailedServerCount", desiredServerSet.size());
	}

	bool shouldHandleServer(const StorageServerInterface& newServer) const {
		return (includedDCs.empty() ||
		        std::find(includedDCs.begin(), includedDCs.end(), newServer.locality.dcId()) != includedDCs.end() ||
		        (otherTrackedDCs.present() &&
		         std::find(otherTrackedDCs.get().begin(), otherTrackedDCs.get().end(), newServer.locality.dcId()) ==
		             otherTrackedDCs.get().end()));
	}

	void addServer(StorageServerInterface newServer,
	               ProcessClass processClass,
	               Promise<Void> errorOut,
	               Version addedVersion,
	               const DDEnabledState* ddEnabledState) {
		if (!shouldHandleServer(newServer)) {
			return;
		}

		if (!newServer.isTss()) {
			allServers.push_back(newServer.id());
		}

		TraceEvent(newServer.isTss() ? "AddedTSS" : "AddedStorageServer", distributorId)
		    .detail("ServerID", newServer.id())
		    .detail("ProcessID", newServer.locality.processId())
		    .detail("ProcessClass", processClass.toString())
		    .detail("WaitFailureToken", newServer.waitFailure.getEndpoint().token)
		    .detail("Address", newServer.waitFailure.getEndpoint().getPrimaryAddress());

		auto& r = server_and_tss_info[newServer.id()] = makeReference<TCServerInfo>(
		    newServer,
		    this,
		    processClass,
		    includedDCs.empty() ||
		        std::find(includedDCs.begin(), includedDCs.end(), newServer.locality.dcId()) != includedDCs.end(),
		    storageServerSet,
		    addedVersion);

		if (newServer.isTss()) {
			tss_info_by_pair[newServer.tssPairID.get()] = r;

			if (server_info.count(newServer.tssPairID.get())) {
				r->onTSSPairRemoved = server_info[newServer.tssPairID.get()]->onRemoved;
			}
		} else {
			server_info[newServer.id()] = r;
			// Establish the relation between server and machine
			checkAndCreateMachine(r);
			// Add storage server to pid map
			ASSERT(r->lastKnownInterface.locality.processId().present());
			StringRef pid = r->lastKnownInterface.locality.processId().get();
			pid2server_info[pid].push_back(r);
		}

		r->tracker =
		    storageServerTracker(this, cx, r.getPtr(), errorOut, addedVersion, ddEnabledState, newServer.isTss());

		if (!newServer.isTss()) {
			// link and wake up tss' tracker so it knows when this server gets removed
			if (tss_info_by_pair.count(newServer.id())) {
				tss_info_by_pair[newServer.id()]->onTSSPairRemoved = r->onRemoved;
				if (tss_info_by_pair[newServer.id()]->wakeUpTracker.canBeSet()) {
					auto p = tss_info_by_pair[newServer.id()]->wakeUpTracker;
					// This callback could delete tss_info_by_pair[newServer.id()], so use a copy
					p.send(Void());
				}
			}

			doBuildTeams = true; // Adding a new server triggers to build new teams
			restartTeamBuilder.trigger();
		}
	}

	bool removeTeam(Reference<TCTeamInfo> team) {
		TraceEvent("RemovedServerTeam", distributorId).detail("Team", team->getDesc());
		bool found = false;
		for (int t = 0; t < teams.size(); t++) {
			if (teams[t] == team) {
				teams[t--] = teams.back();
				teams.pop_back();
				found = true;
				break;
			}
		}

		for (const auto& server : team->getServers()) {
			for (int t = 0; t < server->teams.size(); t++) {
				if (server->teams[t] == team) {
					ASSERT(found);
					server->teams[t--] = server->teams.back();
					server->teams.pop_back();
					break; // The teams on a server should never duplicate
				}
			}
		}

		// Remove the team from its machine team
		bool foundInMachineTeam = false;
		for (int t = 0; t < team->machineTeam->serverTeams.size(); ++t) {
			if (team->machineTeam->serverTeams[t] == team) {
				team->machineTeam->serverTeams[t--] = team->machineTeam->serverTeams.back();
				team->machineTeam->serverTeams.pop_back();
				foundInMachineTeam = true;
				break; // The same team is added to the serverTeams only once
			}
		}

		ASSERT_WE_THINK(foundInMachineTeam);
		team->tracker.cancel();
		if (g_network->isSimulated()) {
			// Update server team information for consistency check in simulation
			traceTeamCollectionInfo();
		}
		return found;
	}

	// Check if the server belongs to a machine; if not, create the machine.
	// Establish the two-direction link between server and machine
	Reference<TCMachineInfo> checkAndCreateMachine(Reference<TCServerInfo> server) {
		ASSERT(server.isValid() && server_info.find(server->id) != server_info.end());
		auto& locality = server->lastKnownInterface.locality;
		Standalone<StringRef> machine_id = locality.zoneId().get(); // locality to machine_id with std::string type

		Reference<TCMachineInfo> machineInfo;
		if (machine_info.find(machine_id) == machine_info.end()) {
			// uid is the first storage server process on the machine
			TEST(true); // First storage server in process on the machine
			// For each machine, store the first server's localityEntry into machineInfo for later use.
			LocalityEntry localityEntry = machineLocalityMap.add(locality, &server->id);
			machineInfo = makeReference<TCMachineInfo>(server, localityEntry);
			machine_info.insert(std::make_pair(machine_id, machineInfo));
		} else {
			machineInfo = machine_info.find(machine_id)->second;
			machineInfo->serversOnMachine.push_back(server);
		}
		server->machine = machineInfo;

		return machineInfo;
	}

	// Check if the serverTeam belongs to a machine team; If not, create the machine team
	// Note: This function may make the machine team number larger than the desired machine team number
	Reference<TCMachineTeamInfo> checkAndCreateMachineTeam(Reference<TCTeamInfo> serverTeam) {
		std::vector<Standalone<StringRef>> machineIDs;
		for (auto& server : serverTeam->getServers()) {
			Reference<TCMachineInfo> machine = server->machine;
			machineIDs.push_back(machine->machineID);
		}

		std::sort(machineIDs.begin(), machineIDs.end());
		Reference<TCMachineTeamInfo> machineTeam = findMachineTeam(machineIDs);
		if (!machineTeam.isValid()) { // Create the machine team if it does not exist
			machineTeam = addMachineTeam(machineIDs.begin(), machineIDs.end());
		}

		machineTeam->serverTeams.push_back(serverTeam);

		return machineTeam;
	}

	// Remove the removedMachineInfo machine and any related machine team
	void removeMachine(Reference<TCMachineInfo> removedMachineInfo) {
		// Find machines that share teams with the removed machine
		std::set<Standalone<StringRef>> machinesWithAjoiningTeams;
		for (auto& machineTeam : removedMachineInfo->machineTeams) {
			machinesWithAjoiningTeams.insert(machineTeam->machineIDs.begin(), machineTeam->machineIDs.end());
		}
		machinesWithAjoiningTeams.erase(removedMachineInfo->machineID);
		// For each machine in a machine team with the removed machine,
		// erase shared machine teams from the list of teams.
		for (auto it = machinesWithAjoiningTeams.begin(); it != machinesWithAjoiningTeams.end(); ++it) {
			auto& machineTeams = machine_info[*it]->machineTeams;
			for (int t = 0; t < machineTeams.size(); t++) {
				auto& machineTeam = machineTeams[t];
				if (std::count(machineTeam->machineIDs.begin(),
				               machineTeam->machineIDs.end(),
				               removedMachineInfo->machineID)) {
					machineTeams[t--] = machineTeams.back();
					machineTeams.pop_back();
				}
			}
		}
		removedMachineInfo->machineTeams.clear();

		// Remove global machine team that includes removedMachineInfo
		for (int t = 0; t < machineTeams.size(); t++) {
			auto& machineTeam = machineTeams[t];
			if (std::count(
			        machineTeam->machineIDs.begin(), machineTeam->machineIDs.end(), removedMachineInfo->machineID)) {
				removeMachineTeam(machineTeam);
				// removeMachineTeam will swap the last team in machineTeams vector into [t];
				// t-- to avoid skipping the element
				t--;
			}
		}

		// Remove removedMachineInfo from machine's global info
		machine_info.erase(removedMachineInfo->machineID);
		TraceEvent("MachineLocalityMapUpdate").detail("MachineUIDRemoved", removedMachineInfo->machineID.toString());

		// We do not update macineLocalityMap when a machine is removed because we will do so when we use it in
		// addBestMachineTeams()
		// rebuildMachineLocalityMap();
	}

	// Invariant: Remove a machine team only when the server teams on it has been removed
	// We never actively remove a machine team.
	// A machine team is removed when a machine is removed,
	// which is caused by the event when all servers on the machine is removed.
	// NOTE: When this function is called in the loop of iterating machineTeams, make sure NOT increase the index
	// in the next iteration of the loop. Otherwise, you may miss checking some elements in machineTeams
	bool removeMachineTeam(Reference<TCMachineTeamInfo> targetMT) {
		bool foundMachineTeam = false;
		for (int i = 0; i < machineTeams.size(); i++) {
			Reference<TCMachineTeamInfo> mt = machineTeams[i];
			if (mt->machineIDs == targetMT->machineIDs) {
				machineTeams[i--] = machineTeams.back();
				machineTeams.pop_back();
				foundMachineTeam = true;
				break;
			}
		}
		// Remove machine team on each machine
		for (auto& machine : targetMT->machines) {
			for (int i = 0; i < machine->machineTeams.size(); ++i) {
				if (machine->machineTeams[i]->machineIDs == targetMT->machineIDs) {
					machine->machineTeams[i--] = machine->machineTeams.back();
					machine->machineTeams.pop_back();
					break; // The machineTeams on a machine should never duplicate
				}
			}
		}

		return foundMachineTeam;
	}

	void removeTSS(UID removedServer) {
		// much simpler than remove server. tss isn't in any teams, so just remove it from data structures
		TraceEvent("RemovedTSS", distributorId).detail("ServerID", removedServer);
		Reference<TCServerInfo> removedServerInfo = server_and_tss_info[removedServer];

		tss_info_by_pair.erase(removedServerInfo->lastKnownInterface.tssPairID.get());
		server_and_tss_info.erase(removedServer);

		server_status.clear(removedServer);
	}

	void removeServer(UID removedServer) {
		TraceEvent("RemovedStorageServer", distributorId).detail("ServerID", removedServer);

		// ASSERT( !shardsAffectedByTeamFailure->getServersForTeam( t ) for all t in teams that contain removedServer )
		Reference<TCServerInfo> removedServerInfo = server_info[removedServer];
		// Step: Remove TCServerInfo from pid2server_info
		ASSERT(removedServerInfo->lastKnownInterface.locality.processId().present());
		StringRef pid = removedServerInfo->lastKnownInterface.locality.processId().get();
		auto& info_vec = pid2server_info[pid];
		for (size_t i = 0; i < info_vec.size(); ++i) {
			if (info_vec[i] == removedServerInfo) {
				info_vec[i--] = info_vec.back();
				info_vec.pop_back();
			}
		}
		if (info_vec.size() == 0) {
			pid2server_info.erase(pid);
		}

		// Step: Remove server team that relate to removedServer
		// Find all servers with which the removedServer shares teams
		std::set<UID> serversWithAjoiningTeams;
		auto& sharedTeams = removedServerInfo->teams;
		for (int i = 0; i < sharedTeams.size(); ++i) {
			auto& teamIds = sharedTeams[i]->getServerIDs();
			serversWithAjoiningTeams.insert(teamIds.begin(), teamIds.end());
		}
		serversWithAjoiningTeams.erase(removedServer);

		// For each server in a team with the removedServer, erase shared teams from the list of teams in that other
		// server
		for (auto it = serversWithAjoiningTeams.begin(); it != serversWithAjoiningTeams.end(); ++it) {
			auto& serverTeams = server_info[*it]->teams;
			for (int t = 0; t < serverTeams.size(); t++) {
				auto& serverIds = serverTeams[t]->getServerIDs();
				if (std::count(serverIds.begin(), serverIds.end(), removedServer)) {
					serverTeams[t--] = serverTeams.back();
					serverTeams.pop_back();
				}
			}
		}

		// Step: Remove all teams that contain removedServer
		// SOMEDAY: can we avoid walking through all teams, since we have an index of teams in which removedServer
		// participated
		int removedCount = 0;
		for (int t = 0; t < teams.size(); t++) {
			if (std::count(teams[t]->getServerIDs().begin(), teams[t]->getServerIDs().end(), removedServer)) {
				TraceEvent("ServerTeamRemoved")
				    .detail("Primary", primary)
				    .detail("TeamServerIDs", teams[t]->getServerIDsStr())
				    .detail("TeamID", teams[t]->getTeamID());
				// removeTeam also needs to remove the team from the machine team info.
				removeTeam(teams[t]);
				t--;
				removedCount++;
			}
		}

		if (removedCount == 0) {
			TraceEvent(SevInfo, "NoTeamsRemovedWhenServerRemoved")
			    .detail("Primary", primary)
			    .detail("Debug", "ThisShouldRarelyHappen_CheckInfoBelow");
		}

		for (int t = 0; t < badTeams.size(); t++) {
			if (std::count(badTeams[t]->getServerIDs().begin(), badTeams[t]->getServerIDs().end(), removedServer)) {
				badTeams[t]->tracker.cancel();
				badTeams[t--] = badTeams.back();
				badTeams.pop_back();
			}
		}

		// Step: Remove machine info related to removedServer
		// Remove the server from its machine
		Reference<TCMachineInfo> removedMachineInfo = removedServerInfo->machine;
		for (int i = 0; i < removedMachineInfo->serversOnMachine.size(); ++i) {
			if (removedMachineInfo->serversOnMachine[i] == removedServerInfo) {
				// Safe even when removedServerInfo is the last one
				removedMachineInfo->serversOnMachine[i--] = removedMachineInfo->serversOnMachine.back();
				removedMachineInfo->serversOnMachine.pop_back();
				break;
			}
		}
		// Remove machine if no server on it
		// Note: Remove machine (and machine team) after server teams have been removed, because
		// we remove a machine team only when the server teams on it have been removed
		if (removedMachineInfo->serversOnMachine.size() == 0) {
			removeMachine(removedMachineInfo);
		}

		// If the machine uses removedServer's locality and the machine still has servers, the the machine's
		// representative server will be updated when it is used in addBestMachineTeams()
		// Note that since we do not rebuildMachineLocalityMap() here, the machineLocalityMap can be stale.
		// This is ok as long as we do not arbitrarily validate if machine team satisfies replication policy.

		if (server_info[removedServer]->wrongStoreTypeToRemove.get()) {
			if (wrongStoreTypeRemover.isReady()) {
				wrongStoreTypeRemover = removeWrongStoreType(this);
				addActor.send(wrongStoreTypeRemover);
			}
		}

		// Step: Remove removedServer from server's global data
		for (int s = 0; s < allServers.size(); s++) {
			if (allServers[s] == removedServer) {
				allServers[s--] = allServers.back();
				allServers.pop_back();
			}
		}
		server_info.erase(removedServer);
		server_and_tss_info.erase(removedServer);

		if (server_status.get(removedServer).initialized && server_status.get(removedServer).isUnhealthy()) {
			unhealthyServers--;
		}
		server_status.clear(removedServer);

		// FIXME: add remove support to localitySet so we do not have to recreate it
		resetLocalitySet();

		doBuildTeams = true;
		restartTeamBuilder.trigger();

		TraceEvent("DataDistributionTeamCollectionUpdate", distributorId)
		    .detail("ServerTeams", teams.size())
		    .detail("BadServerTeams", badTeams.size())
		    .detail("Servers", allServers.size())
		    .detail("Machines", machine_info.size())
		    .detail("MachineTeams", machineTeams.size())
		    .detail("DesiredTeamsPerServer", SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER);
	}

	// Adds storage servers held on process of which the Process Id is “pid” into excludeServers which prevent
	// recruiting the wiggling storage servers and let teamTracker start to move data off the affected teams;
	// Return a vector of futures wait for all data is moved to other teams.
	std::vector<Future<Void>> excludeStorageServersForWiggle(const Value& pid) {
		std::vector<Future<Void>> moveFutures;
		if (this->pid2server_info.count(pid) != 0) {
			for (auto& info : this->pid2server_info[pid]) {
				AddressExclusion addr(info->lastKnownInterface.address().ip, info->lastKnownInterface.address().port);
				if (this->excludedServers.count(addr) &&
				    this->excludedServers.get(addr) != DDTeamCollection::Status::NONE) {
					continue; // don't overwrite the value set by actor trackExcludedServer
				}
				this->wiggle_addresses.push_back(addr);
				this->excludedServers.set(addr, DDTeamCollection::Status::WIGGLING);
				moveFutures.push_back(info->onRemoved);
			}
			if (!moveFutures.empty()) {
				this->restartRecruiting.trigger();
			}
		}
		return moveFutures;
	}

	// Include wiggled storage servers by setting their status from `WIGGLING`
	// to `NONE`. The storage recruiter will recruit them as new storage servers
	void includeStorageServersForWiggle() {
		bool included = false;
		for (auto& address : this->wiggle_addresses) {
			if (!this->excludedServers.count(address) ||
			    this->excludedServers.get(address) != DDTeamCollection::Status::WIGGLING) {
				continue;
			}
			included = true;
			this->excludedServers.set(address, DDTeamCollection::Status::NONE);
		}
		this->wiggle_addresses.clear();
		if (included) {
			this->restartRecruiting.trigger();
		}
	}
};

TCServerInfo::~TCServerInfo() {
	if (collection && ssVersionTooFarBehind.get() && !lastKnownInterface.isTss()) {
		collection->removeLaggingStorageServer(lastKnownInterface.locality.zoneId().get());
	}
}

ACTOR Future<Void> updateServerMetrics(TCServerInfo* server) {
	state StorageServerInterface ssi = server->lastKnownInterface;
	state Future<ErrorOr<GetStorageMetricsReply>> metricsRequest =
	    ssi.getStorageMetrics.tryGetReply(GetStorageMetricsRequest(), TaskPriority::DataDistributionLaunch);
	state Future<Void> resetRequest = Never();
	state Future<std::pair<StorageServerInterface, ProcessClass>> interfaceChanged(server->onInterfaceChanged);
	state Future<Void> serverRemoved(server->onRemoved);

	loop {
		choose {
			when(ErrorOr<GetStorageMetricsReply> rep = wait(metricsRequest)) {
				if (rep.present()) {
					server->serverMetrics = rep;
					if (server->updated.canBeSet()) {
						server->updated.send(Void());
					}
					break;
				}
				metricsRequest = Never();
				resetRequest = delay(SERVER_KNOBS->METRIC_DELAY, TaskPriority::DataDistributionLaunch);
			}
			when(std::pair<StorageServerInterface, ProcessClass> _ssi = wait(interfaceChanged)) {
				ssi = _ssi.first;
				interfaceChanged = server->onInterfaceChanged;
				resetRequest = Void();
			}
			when(wait(serverRemoved)) { return Void(); }
			when(wait(resetRequest)) { // To prevent a tight spin loop
				if (IFailureMonitor::failureMonitor().getState(ssi.getStorageMetrics.getEndpoint()).isFailed()) {
					resetRequest = IFailureMonitor::failureMonitor().onStateEqual(ssi.getStorageMetrics.getEndpoint(),
					                                                              FailureStatus(false));
				} else {
					resetRequest = Never();
					metricsRequest = ssi.getStorageMetrics.tryGetReply(GetStorageMetricsRequest(),
					                                                   TaskPriority::DataDistributionLaunch);
				}
			}
		}
	}

	if (server->serverMetrics.get().lastUpdate < now() - SERVER_KNOBS->DD_SS_STUCK_TIME_LIMIT) {
		if (server->ssVersionTooFarBehind.get() == false) {
			TraceEvent("StorageServerStuck", server->collection->distributorId)
			    .detail("ServerId", server->id.toString())
			    .detail("LastUpdate", server->serverMetrics.get().lastUpdate);
			server->ssVersionTooFarBehind.set(true);
			server->collection->addLaggingStorageServer(server->lastKnownInterface.locality.zoneId().get());
		}
	} else if (server->serverMetrics.get().versionLag > SERVER_KNOBS->DD_SS_FAILURE_VERSIONLAG) {
		if (server->ssVersionTooFarBehind.get() == false) {
			TraceEvent(SevWarn, "SSVersionDiffLarge", server->collection->distributorId)
			    .detail("ServerId", server->id.toString())
			    .detail("VersionLag", server->serverMetrics.get().versionLag);
			server->ssVersionTooFarBehind.set(true);
			server->collection->addLaggingStorageServer(server->lastKnownInterface.locality.zoneId().get());
		}
	} else if (server->serverMetrics.get().versionLag < SERVER_KNOBS->DD_SS_ALLOWED_VERSIONLAG) {
		if (server->ssVersionTooFarBehind.get() == true) {
			TraceEvent("SSVersionDiffNormal", server->collection->distributorId)
			    .detail("ServerId", server->id.toString())
			    .detail("VersionLag", server->serverMetrics.get().versionLag);
			server->ssVersionTooFarBehind.set(false);
			server->collection->removeLaggingStorageServer(server->lastKnownInterface.locality.zoneId().get());
		}
	}
	return Void();
}

ACTOR Future<Void> updateServerMetrics(Reference<TCServerInfo> server) {
	wait(updateServerMetrics(server.getPtr()));
	return Void();
}

ACTOR Future<Void> waitUntilHealthy(DDTeamCollection* self, double extraDelay = 0) {
	state int waitCount = 0;
	loop {
		while (self->zeroHealthyTeams->get() || self->processingUnhealthy->get()) {
			// processingUnhealthy: true when there exists data movement
			TraceEvent("WaitUntilHealthyStalled", self->distributorId)
			    .detail("Primary", self->primary)
			    .detail("ZeroHealthy", self->zeroHealthyTeams->get())
			    .detail("ProcessingUnhealthy", self->processingUnhealthy->get());
			wait(self->zeroHealthyTeams->onChange() || self->processingUnhealthy->onChange());
			waitCount = 0;
		}
		wait(delay(SERVER_KNOBS->DD_STALL_CHECK_DELAY,
		           TaskPriority::Low)); // After the team trackers wait on the initial failure reaction delay, they
		                                // yield. We want to make sure every tracker has had the opportunity to send
		                                // their relocations to the queue.
		if (!self->zeroHealthyTeams->get() && !self->processingUnhealthy->get()) {
			if (extraDelay <= 0.01 || waitCount >= 1) {
				// Return healthy if we do not need extraDelay or when DD are healthy in at least two consecutive check
				return Void();
			} else {
				wait(delay(extraDelay, TaskPriority::Low));
				waitCount++;
			}
		}
	}
}

// Take a snapshot of necessary data structures from `DDTeamCollection` and print them out with yields to avoid slow
// task on the run loop.
ACTOR Future<Void> printSnapshotTeamsInfo(Reference<DDTeamCollection> self) {
	state DatabaseConfiguration configuration;
	state std::map<UID, Reference<TCServerInfo>> server_info;
	state std::map<UID, ServerStatus> server_status;
	state std::vector<Reference<TCTeamInfo>> teams;
	state std::map<Standalone<StringRef>, Reference<TCMachineInfo>> machine_info;
	state std::vector<Reference<TCMachineTeamInfo>> machineTeams;
	// state std::vector<std::string> internedLocalityRecordKeyNameStrings;
	// state int machineLocalityMapEntryArraySize;
	// state std::vector<Reference<LocalityRecord>> machineLocalityMapRecordArray;
	state int traceEventsPrinted = 0;
	state std::vector<const UID*> serverIDs;
	state double lastPrintTime = 0;
	state ReadYourWritesTransaction tr(self->cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			state Future<Void> watchFuture = tr.watch(triggerDDTeamInfoPrintKey);
			wait(tr.commit());
			wait(self->printDetailedTeamsInfo.onTrigger() || watchFuture);
			tr.reset();
			if (now() - lastPrintTime < SERVER_KNOBS->DD_TEAMS_INFO_PRINT_INTERVAL) {
				continue;
			}
			lastPrintTime = now();

			traceEventsPrinted = 0;

			double snapshotStart = now();

			configuration = self->configuration;
			server_info = self->server_info;
			teams = self->teams;
			// Perform deep copy so we have a consistent snapshot, even if yields are performed
			for (const auto& [machineId, info] : self->machine_info) {
				machine_info.emplace(machineId, info->clone());
			}
			machineTeams = self->machineTeams;
			// internedLocalityRecordKeyNameStrings = self->machineLocalityMap._keymap->_lookuparray;
			// machineLocalityMapEntryArraySize = self->machineLocalityMap.size();
			// machineLocalityMapRecordArray = self->machineLocalityMap.getRecordArray();
			std::vector<const UID*> _uids = self->machineLocalityMap.getObjects();
			serverIDs = _uids;

			auto const& keys = self->server_status.getKeys();
			for (auto const& key : keys) {
				server_status.emplace(key, self->server_status.get(key));
			}

			TraceEvent("DDPrintSnapshotTeasmInfo", self->distributorId)
			    .detail("SnapshotSpeed", now() - snapshotStart)
			    .detail("Primary", self->primary);

			// Print to TraceEvents
			TraceEvent("DDConfig", self->distributorId)
			    .detail("StorageTeamSize", configuration.storageTeamSize)
			    .detail("DesiredTeamsPerServer", SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER)
			    .detail("MaxTeamsPerServer", SERVER_KNOBS->MAX_TEAMS_PER_SERVER)
			    .detail("Primary", self->primary);

			TraceEvent("ServerInfo", self->distributorId)
			    .detail("Size", server_info.size())
			    .detail("Primary", self->primary);
			state int i;
			state std::map<UID, Reference<TCServerInfo>>::iterator server = server_info.begin();
			for (i = 0; i < server_info.size(); i++) {
				TraceEvent("ServerInfo", self->distributorId)
				    .detail("ServerInfoIndex", i)
				    .detail("ServerID", server->first.toString())
				    .detail("ServerTeamOwned", server->second->teams.size())
				    .detail("MachineID", server->second->machine->machineID.contents().toString())
				    .detail("Primary", self->primary);
				server++;
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			server = server_info.begin();
			for (i = 0; i < server_info.size(); i++) {
				const UID& uid = server->first;
				TraceEvent("ServerStatus", self->distributorId)
				    .detail("ServerUID", uid)
				    .detail("Healthy", !get(server_status, uid).isUnhealthy())
				    .detail("MachineIsValid", server_info[uid]->machine.isValid())
				    .detail("MachineTeamSize",
				            server_info[uid]->machine.isValid() ? server_info[uid]->machine->machineTeams.size() : -1)
				    .detail("Primary", self->primary);
				server++;
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			TraceEvent("ServerTeamInfo", self->distributorId)
			    .detail("Size", teams.size())
			    .detail("Primary", self->primary);
			for (i = 0; i < teams.size(); i++) {
				const auto& team = teams[i];
				TraceEvent("ServerTeamInfo", self->distributorId)
				    .detail("TeamIndex", i)
				    .detail("Healthy", team->isHealthy())
				    .detail("TeamSize", team->size())
				    .detail("MemberIDs", team->getServerIDsStr())
				    .detail("Primary", self->primary);
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			TraceEvent("MachineInfo", self->distributorId)
			    .detail("Size", machine_info.size())
			    .detail("Primary", self->primary);
			state std::map<Standalone<StringRef>, Reference<TCMachineInfo>>::iterator machine = machine_info.begin();
			state bool isMachineHealthy = false;
			for (i = 0; i < machine_info.size(); i++) {
				Reference<TCMachineInfo> _machine = machine->second;
				if (!_machine.isValid() || machine_info.find(_machine->machineID) == machine_info.end() ||
				    _machine->serversOnMachine.empty()) {
					isMachineHealthy = false;
				}

				// Healthy machine has at least one healthy server
				for (auto& server : _machine->serversOnMachine) {
					if (!get(server_status, server->id).isUnhealthy()) {
						isMachineHealthy = true;
					}
				}

				isMachineHealthy = false;
				TraceEvent("MachineInfo", self->distributorId)
				    .detail("MachineInfoIndex", i)
				    .detail("Healthy", isMachineHealthy)
				    .detail("MachineID", machine->first.contents().toString())
				    .detail("MachineTeamOwned", machine->second->machineTeams.size())
				    .detail("ServerNumOnMachine", machine->second->serversOnMachine.size())
				    .detail("ServersID", machine->second->getServersIDStr())
				    .detail("Primary", self->primary);
				machine++;
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			TraceEvent("MachineTeamInfo", self->distributorId)
			    .detail("Size", machineTeams.size())
			    .detail("Primary", self->primary);
			for (i = 0; i < machineTeams.size(); i++) {
				const auto& team = machineTeams[i];
				TraceEvent("MachineTeamInfo", self->distributorId)
				    .detail("TeamIndex", i)
				    .detail("MachineIDs", team->getMachineIDsStr())
				    .detail("ServerTeams", team->serverTeams.size())
				    .detail("Primary", self->primary);
				if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
					wait(yield());
				}
			}

			// TODO: re-enable the following logging or remove them.
			// TraceEvent("LocalityRecordKeyName", self->distributorId)
			//     .detail("Size", internedLocalityRecordKeyNameStrings.size())
			//     .detail("Primary", self->primary);
			// for (i = 0; i < internedLocalityRecordKeyNameStrings.size(); i++) {
			// 	TraceEvent("LocalityRecordKeyIndexName", self->distributorId)
			// 	    .detail("KeyIndex", i)
			// 	    .detail("KeyName", internedLocalityRecordKeyNameStrings[i])
			// 	    .detail("Primary", self->primary);
			// 	if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
			// 		wait(yield());
			// 	}
			// }

			// TraceEvent("MachineLocalityMap", self->distributorId)
			//     .detail("Size", machineLocalityMapEntryArraySize)
			//     .detail("Primary", self->primary);
			// for (i = 0; i < serverIDs.size(); i++) {
			// 	const auto& serverID = serverIDs[i];
			// 	Reference<LocalityRecord> record = machineLocalityMapRecordArray[i];
			// 	if (record.isValid()) {
			// 		TraceEvent("MachineLocalityMap", self->distributorId)
			// 		    .detail("LocalityIndex", i)
			// 		    .detail("UID", serverID->toString())
			// 		    .detail("LocalityRecord", record->toString())
			// 		    .detail("Primary", self->primary);
			// 	} else {
			// 		TraceEvent("MachineLocalityMap", self->distributorId)
			// 		    .detail("LocalityIndex", i)
			// 		    .detail("UID", serverID->toString())
			// 		    .detail("LocalityRecord", "[NotFound]")
			// 		    .detail("Primary", self->primary);
			// 	}
			// 	if (++traceEventsPrinted % SERVER_KNOBS->DD_TEAMS_INFO_PRINT_YIELD_COUNT == 0) {
			// 		wait(yield());
			// 	}
			// }
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<Void> removeBadTeams(DDTeamCollection* self) {
	wait(self->initialFailureReactionDelay);
	wait(waitUntilHealthy(self));
	wait(self->addSubsetComplete.getFuture());
	TraceEvent("DDRemovingBadServerTeams", self->distributorId).detail("Primary", self->primary);
	for (auto it : self->badTeams) {
		it->tracker.cancel();
	}
	self->badTeams.clear();
	return Void();
}

bool isCorrectDC(DDTeamCollection* self, TCServerInfo* server) {
	return (self->includedDCs.empty() ||
	        std::find(self->includedDCs.begin(), self->includedDCs.end(), server->lastKnownInterface.locality.dcId()) !=
	            self->includedDCs.end());
}

ACTOR Future<Void> removeWrongStoreType(DDTeamCollection* self) {
	// Wait for storage servers to initialize its storeType
	wait(delay(SERVER_KNOBS->DD_REMOVE_STORE_ENGINE_DELAY));

	state Future<Void> fisServerRemoved = Never();

	TraceEvent("WrongStoreTypeRemoverStart", self->distributorId).detail("Servers", self->server_info.size());
	loop {
		// Removing a server here when DD is not healthy may lead to rare failure scenarios, for example,
		// the server with wrong storeType is shutting down while this actor marks it as to-be-removed.
		// In addition, removing servers cause extra data movement, which should be done while a cluster is healthy
		wait(waitUntilHealthy(self));

		bool foundSSToRemove = false;

		for (auto& server : self->server_info) {
			if (!server.second->isCorrectStoreType(self->configuration.storageServerStoreType)) {
				// Server may be removed due to failure while the wrongStoreTypeToRemove is sent to the
				// storageServerTracker. This race may cause the server to be removed before react to
				// wrongStoreTypeToRemove
				if (self->configuration.storageMigrationType == StorageMigrationType::AGGRESSIVE) {
					// if the Storage Migration type is aggressive, let DD remove SS with wrong storage type
					server.second->wrongStoreTypeToRemove.set(true);
				}
				// Otherwise, wait Perpetual Wiggler to wiggle the SS with wrong storage type
				foundSSToRemove = true;
				TraceEvent("WrongStoreTypeRemover", self->distributorId)
				    .detail("Server", server.first)
				    .detail("StoreType", server.second->storeType)
				    .detail("ConfiguredStoreType", self->configuration.storageServerStoreType);
				break;
			}
		}

		if (!foundSSToRemove) {
			break;
		}
	}

	return Void();
}

ACTOR Future<Void> machineTeamRemover(DDTeamCollection* self) {
	state int numMachineTeamRemoved = 0;
	loop {
		// In case the machineTeamRemover cause problems in production, we can disable it
		if (SERVER_KNOBS->TR_FLAG_DISABLE_MACHINE_TEAM_REMOVER) {
			return Void(); // Directly return Void()
		}

		// To avoid removing machine teams too fast, which is unlikely happen though
		wait(delay(SERVER_KNOBS->TR_REMOVE_MACHINE_TEAM_DELAY, TaskPriority::DataDistribution));

		wait(waitUntilHealthy(self, SERVER_KNOBS->TR_REMOVE_SERVER_TEAM_EXTRA_DELAY));
		// Wait for the badTeamRemover() to avoid the potential race between adding the bad team (add the team tracker)
		// and remove bad team (cancel the team tracker).
		wait(self->badTeamRemover);

		state int healthyMachineCount = self->calculateHealthyMachineCount();
		// Check if all machines are healthy, if not, we wait for 1 second and loop back.
		// Eventually, all machines will become healthy.
		if (healthyMachineCount != self->machine_info.size()) {
			continue;
		}

		// From this point, all machine teams and server teams should be healthy, because we wait above
		// until processingUnhealthy is done, and all machines are healthy

		// Sanity check all machine teams are healthy
		//		int currentHealthyMTCount = self->getHealthyMachineTeamCount();
		//		if (currentHealthyMTCount != self->machineTeams.size()) {
		//			TraceEvent(SevError, "InvalidAssumption")
		//			    .detail("HealthyMachineCount", healthyMachineCount)
		//			    .detail("Machines", self->machine_info.size())
		//			    .detail("CurrentHealthyMTCount", currentHealthyMTCount)
		//			    .detail("MachineTeams", self->machineTeams.size());
		//			self->traceAllInfo(true);
		//		}

		// In most cases, all machine teams should be healthy teams at this point.
		int desiredMachineTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * healthyMachineCount;
		int totalMTCount = self->machineTeams.size();
		// Pick the machine team to remove. After release-6.2 version,
		// we remove the machine team with most machine teams, the same logic as serverTeamRemover
		std::pair<Reference<TCMachineTeamInfo>, int> foundMTInfo = SERVER_KNOBS->TR_FLAG_REMOVE_MT_WITH_MOST_TEAMS
		                                                               ? self->getMachineTeamWithMostMachineTeams()
		                                                               : self->getMachineTeamWithLeastProcessTeams();

		if (totalMTCount > desiredMachineTeams && foundMTInfo.first.isValid()) {
			Reference<TCMachineTeamInfo> mt = foundMTInfo.first;
			int minNumProcessTeams = foundMTInfo.second;
			ASSERT(mt.isValid());

			// Pick one process team, and mark it as a bad team
			// Remove the machine by removing its process team one by one
			Reference<TCTeamInfo> team;
			int teamIndex = 0;
			for (teamIndex = 0; teamIndex < mt->serverTeams.size(); ++teamIndex) {
				team = mt->serverTeams[teamIndex];
				ASSERT(team->machineTeam->machineIDs == mt->machineIDs); // Sanity check

				// Check if a server will have 0 team after the team is removed
				for (auto& s : team->getServers()) {
					if (s->teams.size() == 0) {
						TraceEvent(SevError, "MachineTeamRemoverTooAggressive", self->distributorId)
						    .detail("Server", s->id)
						    .detail("ServerTeam", team->getDesc());
						self->traceAllInfo(true);
					}
				}

				// The team will be marked as a bad team
				bool foundTeam = self->removeTeam(team);
				ASSERT(foundTeam == true);
				// removeTeam() has side effect of swapping the last element to the current pos
				// in the serverTeams vector in the machine team.
				--teamIndex;
				self->addTeam(team->getServers(), true, true);
				TEST(true); // Removed machine team
			}

			self->doBuildTeams = true;

			if (self->badTeamRemover.isReady()) {
				self->badTeamRemover = removeBadTeams(self);
				self->addActor.send(self->badTeamRemover);
			}

			TraceEvent("MachineTeamRemover", self->distributorId)
			    .detail("MachineTeamIDToRemove", mt->id.shortString())
			    .detail("MachineTeamToRemove", mt->getMachineIDsStr())
			    .detail("NumProcessTeamsOnTheMachineTeam", minNumProcessTeams)
			    .detail("CurrentMachineTeams", self->machineTeams.size())
			    .detail("DesiredMachineTeams", desiredMachineTeams);

			// Remove the machine team
			bool foundRemovedMachineTeam = self->removeMachineTeam(mt);
			// When we remove the last server team on a machine team in removeTeam(), we also remove the machine team
			// This is needed for removeTeam() functoin.
			// So here the removeMachineTeam() should not find the machine team
			ASSERT(foundRemovedMachineTeam);
			numMachineTeamRemoved++;
		} else {
			if (numMachineTeamRemoved > 0) {
				// Only trace the information when we remove a machine team
				TraceEvent("MachineTeamRemoverDone", self->distributorId)
				    .detail("HealthyMachines", healthyMachineCount)
				    // .detail("CurrentHealthyMachineTeams", currentHealthyMTCount)
				    .detail("CurrentMachineTeams", self->machineTeams.size())
				    .detail("DesiredMachineTeams", desiredMachineTeams)
				    .detail("NumMachineTeamsRemoved", numMachineTeamRemoved);
				self->traceTeamCollectionInfo();
				numMachineTeamRemoved = 0; // Reset the counter to avoid keep printing the message
			}
		}
	}
}

// Remove the server team whose members have the most number of process teams
// until the total number of server teams is no larger than the desired number
ACTOR Future<Void> serverTeamRemover(DDTeamCollection* self) {
	state int numServerTeamRemoved = 0;
	loop {
		// In case the serverTeamRemover cause problems in production, we can disable it
		if (SERVER_KNOBS->TR_FLAG_DISABLE_SERVER_TEAM_REMOVER) {
			return Void(); // Directly return Void()
		}

		double removeServerTeamDelay = SERVER_KNOBS->TR_REMOVE_SERVER_TEAM_DELAY;
		if (g_network->isSimulated()) {
			// Speed up the team remover in simulation; otherwise,
			// it may time out because we need to remove hundreds of teams
			removeServerTeamDelay = removeServerTeamDelay / 100;
		}
		// To avoid removing server teams too fast, which is unlikely happen though
		wait(delay(removeServerTeamDelay, TaskPriority::DataDistribution));

		wait(waitUntilHealthy(self, SERVER_KNOBS->TR_REMOVE_SERVER_TEAM_EXTRA_DELAY));
		// Wait for the badTeamRemover() to avoid the potential race between
		// adding the bad team (add the team tracker) and remove bad team (cancel the team tracker).
		wait(self->badTeamRemover);

		// From this point, all server teams should be healthy, because we wait above
		// until processingUnhealthy is done, and all machines are healthy
		int desiredServerTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * self->server_info.size();
		int totalSTCount = self->teams.size();
		// Pick the server team whose members are on the most number of server teams, and mark it undesired
		std::pair<Reference<TCTeamInfo>, int> foundSTInfo = self->getServerTeamWithMostProcessTeams();

		if (totalSTCount > desiredServerTeams && foundSTInfo.first.isValid()) {
			ASSERT(foundSTInfo.first.isValid());
			Reference<TCTeamInfo> st = foundSTInfo.first;
			int maxNumProcessTeams = foundSTInfo.second;
			ASSERT(st.isValid());
			// The team will be marked as a bad team
			bool foundTeam = self->removeTeam(st);
			ASSERT(foundTeam == true);
			self->addTeam(st->getServers(), true, true);
			TEST(true); // Marked team as a bad team

			self->doBuildTeams = true;

			if (self->badTeamRemover.isReady()) {
				self->badTeamRemover = removeBadTeams(self);
				self->addActor.send(self->badTeamRemover);
			}

			TraceEvent("ServerTeamRemover", self->distributorId)
			    .detail("ServerTeamToRemove", st->getServerIDsStr())
			    .detail("ServerTeamID", st->getTeamID())
			    .detail("NumProcessTeamsOnTheServerTeam", maxNumProcessTeams)
			    .detail("CurrentServerTeams", self->teams.size())
			    .detail("DesiredServerTeams", desiredServerTeams);

			numServerTeamRemoved++;
		} else {
			if (numServerTeamRemoved > 0) {
				// Only trace the information when we remove a machine team
				TraceEvent("ServerTeamRemoverDone", self->distributorId)
				    .detail("CurrentServerTeams", self->teams.size())
				    .detail("DesiredServerTeams", desiredServerTeams)
				    .detail("NumServerTeamRemoved", numServerTeamRemoved);
				self->traceTeamCollectionInfo();
				numServerTeamRemoved = 0; // Reset the counter to avoid keep printing the message
			}
		}
	}
}

ACTOR Future<Void> zeroServerLeftLogger_impl(DDTeamCollection* self, Reference<TCTeamInfo> team) {
	wait(delay(SERVER_KNOBS->DD_TEAM_ZERO_SERVER_LEFT_LOG_DELAY));
	state std::vector<KeyRange> shards = self->shardsAffectedByTeamFailure->getShardsFor(
	    ShardsAffectedByTeamFailure::Team(team->getServerIDs(), self->primary));
	state std::vector<Future<StorageMetrics>> sizes;
	sizes.reserve(shards.size());

	for (auto const& shard : shards) {
		sizes.emplace_back(brokenPromiseToNever(self->getShardMetrics.getReply(GetMetricsRequest(shard))));
		TraceEvent(SevWarnAlways, "DDShardLost", self->distributorId)
		    .detail("ServerTeamID", team->getTeamID())
		    .detail("ShardBegin", shard.begin)
		    .detail("ShardEnd", shard.end);
	}

	wait(waitForAll(sizes));

	int64_t bytesLost = 0;
	for (auto const& size : sizes) {
		bytesLost += size.get().bytes;
	}

	TraceEvent(SevWarnAlways, "DDZeroServerLeftInTeam", self->distributorId)
	    .detail("Team", team->getDesc())
	    .detail("TotalBytesLost", bytesLost);

	return Void();
}

bool teamContainsFailedServer(DDTeamCollection* self, Reference<TCTeamInfo> team) {
	auto ssis = team->getLastKnownServerInterfaces();
	for (const auto& ssi : ssis) {
		AddressExclusion addr(ssi.address().ip, ssi.address().port);
		AddressExclusion ipaddr(ssi.address().ip);
		if (self->excludedServers.get(addr) == DDTeamCollection::Status::FAILED ||
		    self->excludedServers.get(ipaddr) == DDTeamCollection::Status::FAILED) {
			return true;
		}
		if (ssi.secondaryAddress().present()) {
			AddressExclusion saddr(ssi.secondaryAddress().get().ip, ssi.secondaryAddress().get().port);
			AddressExclusion sipaddr(ssi.secondaryAddress().get().ip);
			if (self->excludedServers.get(saddr) == DDTeamCollection::Status::FAILED ||
			    self->excludedServers.get(sipaddr) == DDTeamCollection::Status::FAILED) {
				return true;
			}
		}
	}
	return false;
}

// Track a team and issue RelocateShards when the level of degradation changes
// A badTeam can be unhealthy or just a redundantTeam removed by machineTeamRemover() or serverTeamRemover()
ACTOR Future<Void> teamTracker(DDTeamCollection* self, Reference<TCTeamInfo> team, bool badTeam, bool redundantTeam) {
	state int lastServersLeft = team->size();
	state bool lastAnyUndesired = false;
	state bool lastAnyWigglingServer = false;
	state bool logTeamEvents =
	    g_network->isSimulated() || !badTeam || team->size() <= self->configuration.storageTeamSize;
	state bool lastReady = false;
	state bool lastHealthy;
	state bool lastOptimal;
	state bool lastWrongConfiguration = team->isWrongConfiguration();

	state bool lastZeroHealthy = self->zeroHealthyTeams->get();
	state bool firstCheck = true;

	state Future<Void> zeroServerLeftLogger;

	if (logTeamEvents) {
		TraceEvent("ServerTeamTrackerStarting", self->distributorId)
		    .detail("Reason", "Initial wait complete (sc)")
		    .detail("ServerTeam", team->getDesc());
	}
	self->priority_teams[team->getPriority()]++;

	try {
		loop {
			if (logTeamEvents) {
				TraceEvent("ServerTeamHealthChangeDetected", self->distributorId)
				    .detail("ServerTeam", team->getDesc())
				    .detail("Primary", self->primary)
				    .detail("IsReady", self->initialFailureReactionDelay.isReady());
				self->traceTeamCollectionInfo();
			}

			// Check if the number of degraded machines has changed
			state std::vector<Future<Void>> change;
			bool anyUndesired = false;
			bool anyWrongConfiguration = false;
			bool anyWigglingServer = false;
			int serversLeft = 0, serverUndesired = 0, serverWrongConf = 0, serverWiggling = 0;

			for (const UID& uid : team->getServerIDs()) {
				change.push_back(self->server_status.onChange(uid));
				auto& status = self->server_status.get(uid);
				if (!status.isFailed) {
					serversLeft++;
				}
				if (status.isUndesired) {
					anyUndesired = true;
					serverUndesired++;
				}
				if (status.isWrongConfiguration) {
					anyWrongConfiguration = true;
					serverWrongConf++;
				}
				if (status.isWiggling) {
					anyWigglingServer = true;
					serverWiggling++;
				}
			}

			if (serversLeft == 0) {
				logTeamEvents = true;
			}

			// Failed server should not trigger DD if SS failures are set to be ignored
			if (!badTeam && self->healthyZone.get().present() &&
			    (self->healthyZone.get().get() == ignoreSSFailuresZoneString)) {
				ASSERT_WE_THINK(serversLeft == self->configuration.storageTeamSize);
			}

			if (!self->initialFailureReactionDelay.isReady()) {
				change.push_back(self->initialFailureReactionDelay);
			}
			change.push_back(self->zeroHealthyTeams->onChange());

			bool healthy = !badTeam && !anyUndesired && serversLeft == self->configuration.storageTeamSize;
			team->setHealthy(healthy); // Unhealthy teams won't be chosen by bestTeam
			bool optimal = team->isOptimal() && healthy;
			bool containsFailed = teamContainsFailedServer(self, team);
			bool recheck = !healthy && (lastReady != self->initialFailureReactionDelay.isReady() ||
			                            (lastZeroHealthy && !self->zeroHealthyTeams->get()) || containsFailed);

			// TraceEvent("TeamHealthChangeDetected", self->distributorId)
			//     .detail("Team", team->getDesc())
			//     .detail("ServersLeft", serversLeft)
			//     .detail("LastServersLeft", lastServersLeft)
			//     .detail("AnyUndesired", anyUndesired)
			//     .detail("LastAnyUndesired", lastAnyUndesired)
			//     .detail("AnyWrongConfiguration", anyWrongConfiguration)
			//     .detail("LastWrongConfiguration", lastWrongConfiguration)
			//     .detail("Recheck", recheck)
			//     .detail("BadTeam", badTeam)
			//     .detail("LastZeroHealthy", lastZeroHealthy)
			//     .detail("ZeroHealthyTeam", self->zeroHealthyTeams->get());

			lastReady = self->initialFailureReactionDelay.isReady();
			lastZeroHealthy = self->zeroHealthyTeams->get();

			if (firstCheck) {
				firstCheck = false;
				if (healthy) {
					self->healthyTeamCount++;
					self->zeroHealthyTeams->set(false);
				}
				lastHealthy = healthy;

				if (optimal) {
					self->optimalTeamCount++;
					self->zeroOptimalTeams.set(false);
				}
				lastOptimal = optimal;
			}

			if (serversLeft != lastServersLeft || anyUndesired != lastAnyUndesired ||
			    anyWrongConfiguration != lastWrongConfiguration || anyWigglingServer != lastAnyWigglingServer ||
			    recheck) { // NOTE: do not check wrongSize
				if (logTeamEvents) {
					TraceEvent("ServerTeamHealthChanged", self->distributorId)
					    .detail("ServerTeam", team->getDesc())
					    .detail("ServersLeft", serversLeft)
					    .detail("LastServersLeft", lastServersLeft)
					    .detail("ContainsUndesiredServer", anyUndesired)
					    .detail("ContainsWigglingServer", anyWigglingServer)
					    .detail("HealthyTeamsCount", self->healthyTeamCount)
					    .detail("IsWrongConfiguration", anyWrongConfiguration);
				}

				team->setWrongConfiguration(anyWrongConfiguration);

				if (optimal != lastOptimal) {
					lastOptimal = optimal;
					self->optimalTeamCount += optimal ? 1 : -1;

					ASSERT(self->optimalTeamCount >= 0);
					self->zeroOptimalTeams.set(self->optimalTeamCount == 0);
				}

				if (lastHealthy != healthy) {
					lastHealthy = healthy;
					// Update healthy team count when the team healthy changes
					self->healthyTeamCount += healthy ? 1 : -1;

					ASSERT(self->healthyTeamCount >= 0);
					self->zeroHealthyTeams->set(self->healthyTeamCount == 0);

					if (self->healthyTeamCount == 0) {
						TraceEvent(SevWarn, "ZeroServerTeamsHealthySignalling", self->distributorId)
						    .detail("SignallingTeam", team->getDesc())
						    .detail("Primary", self->primary);
					}

					if (logTeamEvents) {
						TraceEvent("ServerTeamHealthDifference", self->distributorId)
						    .detail("ServerTeam", team->getDesc())
						    .detail("LastOptimal", lastOptimal)
						    .detail("LastHealthy", lastHealthy)
						    .detail("Optimal", optimal)
						    .detail("OptimalTeamCount", self->optimalTeamCount);
					}
				}

				lastServersLeft = serversLeft;
				lastAnyUndesired = anyUndesired;
				lastWrongConfiguration = anyWrongConfiguration;
				lastAnyWigglingServer = anyWigglingServer;

				state int lastPriority = team->getPriority();
				if (team->size() == 0) {
					team->setPriority(SERVER_KNOBS->PRIORITY_POPULATE_REGION);
				} else if (serversLeft < self->configuration.storageTeamSize) {
					if (serversLeft == 0)
						team->setPriority(SERVER_KNOBS->PRIORITY_TEAM_0_LEFT);
					else if (serversLeft == 1)
						team->setPriority(SERVER_KNOBS->PRIORITY_TEAM_1_LEFT);
					else if (serversLeft == 2)
						team->setPriority(SERVER_KNOBS->PRIORITY_TEAM_2_LEFT);
					else
						team->setPriority(SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY);
				} else if (!badTeam && anyWigglingServer && serverWiggling == serverWrongConf &&
				           serverWiggling == serverUndesired) {
					// the wrong configured and undesired server is the wiggling server
					team->setPriority(SERVER_KNOBS->PRIORITY_PERPETUAL_STORAGE_WIGGLE);
				} else if (badTeam || anyWrongConfiguration) {
					if (redundantTeam) {
						team->setPriority(SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT);
					} else {
						team->setPriority(SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY);
					}
				} else if (anyUndesired) {
					team->setPriority(SERVER_KNOBS->PRIORITY_TEAM_CONTAINS_UNDESIRED_SERVER);
				} else {
					team->setPriority(SERVER_KNOBS->PRIORITY_TEAM_HEALTHY);
				}

				if (lastPriority != team->getPriority()) {
					self->priority_teams[lastPriority]--;
					self->priority_teams[team->getPriority()]++;
					if (lastPriority == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT &&
					    team->getPriority() < SERVER_KNOBS->PRIORITY_TEAM_0_LEFT) {
						zeroServerLeftLogger = Void();
					}
					if (logTeamEvents) {
						int dataLoss = team->getPriority() == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT;
						Severity severity = dataLoss ? SevWarnAlways : SevInfo;
						TraceEvent(severity, "ServerTeamPriorityChange", self->distributorId)
						    .detail("Priority", team->getPriority())
						    .detail("Info", team->getDesc())
						    .detail("ZeroHealthyServerTeams", self->zeroHealthyTeams->get())
						    .detail("Hint",
						            severity == SevWarnAlways ? "No replicas remain of some data"
						                                      : "The priority of this team changed");
						if (team->getPriority() == SERVER_KNOBS->PRIORITY_TEAM_0_LEFT) {
							// 0 servers left in this team, data might be lost.
							zeroServerLeftLogger = zeroServerLeftLogger_impl(self, team);
						}
					}
				}

				lastZeroHealthy =
				    self->zeroHealthyTeams->get(); // set this again in case it changed from this teams health changing
				if ((self->initialFailureReactionDelay.isReady() && !self->zeroHealthyTeams->get()) || containsFailed) {

					std::vector<KeyRange> shards = self->shardsAffectedByTeamFailure->getShardsFor(
					    ShardsAffectedByTeamFailure::Team(team->getServerIDs(), self->primary));

					for (int i = 0; i < shards.size(); i++) {
						// Make it high priority to move keys off failed server or else RelocateShards may never be
						// addressed
						int maxPriority = containsFailed ? SERVER_KNOBS->PRIORITY_TEAM_FAILED : team->getPriority();
						// The shard split/merge and DD rebooting may make a shard mapped to multiple teams,
						// so we need to recalculate the shard's priority
						if (maxPriority < SERVER_KNOBS->PRIORITY_TEAM_FAILED) {
							std::pair<std::vector<ShardsAffectedByTeamFailure::Team>,
							          std::vector<ShardsAffectedByTeamFailure::Team>>
							    teams = self->shardsAffectedByTeamFailure->getTeamsFor(shards[i]);
							for (int j = 0; j < teams.first.size() + teams.second.size(); j++) {
								// t is the team in primary DC or the remote DC
								auto& t =
								    j < teams.first.size() ? teams.first[j] : teams.second[j - teams.first.size()];
								if (!t.servers.size()) {
									maxPriority = std::max(maxPriority, SERVER_KNOBS->PRIORITY_POPULATE_REGION);
									break;
								}

								auto tc = self->teamCollections[t.primary ? 0 : 1];
								if (tc == nullptr) {
									// teamTracker only works when all teamCollections are valid.
									// Always check if all teamCollections are valid, and throw error if any
									// teamCollection has been destructed, because the teamTracker can be triggered
									// after a DDTeamCollection was destroyed and before the other DDTeamCollection is
									// destroyed. Do not throw actor_cancelled() because flow treat it differently.
									throw dd_cancelled();
								}
								ASSERT(tc->primary == t.primary);
								// tc->traceAllInfo();
								if (tc->server_info.count(t.servers[0])) {
									auto& info = tc->server_info[t.servers[0]];

									bool found = false;
									for (int k = 0; k < info->teams.size(); k++) {
										if (info->teams[k]->getServerIDs() == t.servers) {
											maxPriority = std::max(maxPriority, info->teams[k]->getPriority());
											found = true;

											break;
										}
									}

									// If we cannot find the team, it could be a bad team so assume unhealthy priority
									if (!found) {
										// If the input team (in function parameters) is a redundant team, found will be
										// false We want to differentiate the redundant_team from unhealthy_team in
										// terms of relocate priority
										maxPriority =
										    std::max<int>(maxPriority,
										                  redundantTeam ? SERVER_KNOBS->PRIORITY_TEAM_REDUNDANT
										                                : SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY);
									}
								} else {
									TEST(true); // A removed server is still associated with a team in
									            // ShardsAffectedByTeamFailure
								}
							}
						}

						RelocateShard rs;
						rs.keys = shards[i];
						rs.priority = maxPriority;

						self->output.send(rs);
						TraceEvent("SendRelocateToDDQueue", self->distributorId)
						    .suppressFor(1.0)
						    .detail("ServerPrimary", self->primary)
						    .detail("ServerTeam", team->getDesc())
						    .detail("KeyBegin", rs.keys.begin)
						    .detail("KeyEnd", rs.keys.end)
						    .detail("Priority", rs.priority)
						    .detail("ServerTeamFailedMachines", team->size() - serversLeft)
						    .detail("ServerTeamOKMachines", serversLeft);
					}
				} else {
					if (logTeamEvents) {
						TraceEvent("ServerTeamHealthNotReady", self->distributorId)
						    .detail("HealthyServerTeamCount", self->healthyTeamCount)
						    .detail("ServerTeamID", team->getTeamID());
					}
				}
			}

			// Wait for any of the machines to change status
			wait(quorum(change, 1));
			wait(yield());
		}
	} catch (Error& e) {
		if (logTeamEvents) {
			TraceEvent("TeamTrackerStopping", self->distributorId)
			    .detail("ServerPrimary", self->primary)
			    .detail("Team", team->getDesc())
			    .detail("Priority", team->getPriority());
		}
		self->priority_teams[team->getPriority()]--;
		if (team->isHealthy()) {
			self->healthyTeamCount--;
			ASSERT(self->healthyTeamCount >= 0);

			if (self->healthyTeamCount == 0) {
				TraceEvent(SevWarn, "ZeroTeamsHealthySignalling", self->distributorId)
				    .detail("ServerPrimary", self->primary)
				    .detail("SignallingServerTeam", team->getDesc());
				self->zeroHealthyTeams->set(true);
			}
		}
		if (lastOptimal) {
			self->optimalTeamCount--;
			ASSERT(self->optimalTeamCount >= 0);
			self->zeroOptimalTeams.set(self->optimalTeamCount == 0);
		}
		throw;
	}
}

ACTOR Future<Void> trackExcludedServers(DDTeamCollection* self) {
	// Fetch the list of excluded servers
	state ReadYourWritesTransaction tr(self->cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			state Future<RangeResult> fresultsExclude = tr.getRange(excludedServersKeys, CLIENT_KNOBS->TOO_MANY);
			state Future<RangeResult> fresultsFailed = tr.getRange(failedServersKeys, CLIENT_KNOBS->TOO_MANY);
			state Future<RangeResult> flocalitiesExclude = tr.getRange(excludedLocalityKeys, CLIENT_KNOBS->TOO_MANY);
			state Future<RangeResult> flocalitiesFailed = tr.getRange(failedLocalityKeys, CLIENT_KNOBS->TOO_MANY);
			state Future<std::vector<ProcessData>> fworkers = getWorkers(self->cx);
			wait(success(fresultsExclude) && success(fresultsFailed) && success(flocalitiesExclude) &&
			     success(flocalitiesFailed));

			state RangeResult excludedResults = fresultsExclude.get();
			ASSERT(!excludedResults.more && excludedResults.size() < CLIENT_KNOBS->TOO_MANY);

			state RangeResult failedResults = fresultsFailed.get();
			ASSERT(!failedResults.more && failedResults.size() < CLIENT_KNOBS->TOO_MANY);

			state RangeResult excludedLocalityResults = flocalitiesExclude.get();
			ASSERT(!excludedLocalityResults.more && excludedLocalityResults.size() < CLIENT_KNOBS->TOO_MANY);

			state RangeResult failedLocalityResults = flocalitiesFailed.get();
			ASSERT(!failedLocalityResults.more && failedLocalityResults.size() < CLIENT_KNOBS->TOO_MANY);

			state std::set<AddressExclusion> excluded;
			state std::set<AddressExclusion> failed;
			for (const auto& r : excludedResults) {
				AddressExclusion addr = decodeExcludedServersKey(r.key);
				if (addr.isValid()) {
					excluded.insert(addr);
				}
			}
			for (const auto& r : failedResults) {
				AddressExclusion addr = decodeFailedServersKey(r.key);
				if (addr.isValid()) {
					failed.insert(addr);
				}
			}

			wait(success(fworkers));
			std::vector<ProcessData> workers = fworkers.get();
			for (const auto& r : excludedLocalityResults) {
				std::string locality = decodeExcludedLocalityKey(r.key);
				std::set<AddressExclusion> localityExcludedAddresses = getAddressesByLocality(workers, locality);
				excluded.insert(localityExcludedAddresses.begin(), localityExcludedAddresses.end());
			}
			for (const auto& r : failedLocalityResults) {
				std::string locality = decodeFailedLocalityKey(r.key);
				std::set<AddressExclusion> localityFailedAddresses = getAddressesByLocality(workers, locality);
				failed.insert(localityFailedAddresses.begin(), localityFailedAddresses.end());
			}

			// Reset and reassign self->excludedServers based on excluded, but we only
			// want to trigger entries that are different
			// Do not retrigger and double-overwrite failed or wiggling servers
			auto old = self->excludedServers.getKeys();
			for (const auto& o : old) {
				if (!excluded.count(o) && !failed.count(o) &&
				    !(self->excludedServers.count(o) &&
				      self->excludedServers.get(o) == DDTeamCollection::Status::WIGGLING)) {
					self->excludedServers.set(o, DDTeamCollection::Status::NONE);
				}
			}
			for (const auto& n : excluded) {
				if (!failed.count(n)) {
					self->excludedServers.set(n, DDTeamCollection::Status::EXCLUDED);
				}
			}

			for (const auto& f : failed) {
				self->excludedServers.set(f, DDTeamCollection::Status::FAILED);
			}

			TraceEvent("DDExcludedServersChanged", self->distributorId)
			    .detail("AddressesExcluded", excludedResults.size())
			    .detail("AddressesFailed", failedResults.size())
			    .detail("LocalitiesExcluded", excludedLocalityResults.size())
			    .detail("LocalitiesFailed", failedLocalityResults.size());

			self->restartRecruiting.trigger();
			state Future<Void> watchFuture = tr.watch(excludedServersVersionKey) || tr.watch(failedServersVersionKey) ||
			                                 tr.watch(excludedLocalityVersionKey) || tr.watch(failedLocalityVersionKey);
			wait(tr.commit());
			wait(watchFuture);
			tr.reset();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<std::vector<std::pair<StorageServerInterface, ProcessClass>>> getServerListAndProcessClasses(
    Transaction* tr) {
	state Future<std::vector<ProcessData>> workers = getWorkers(tr);
	state Future<RangeResult> serverList = tr->getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY);
	wait(success(workers) && success(serverList));
	ASSERT(!serverList.get().more && serverList.get().size() < CLIENT_KNOBS->TOO_MANY);

	std::map<Optional<Standalone<StringRef>>, ProcessData> id_data;
	for (int i = 0; i < workers.get().size(); i++)
		id_data[workers.get()[i].locality.processId()] = workers.get()[i];

	std::vector<std::pair<StorageServerInterface, ProcessClass>> results;
	for (int i = 0; i < serverList.get().size(); i++) {
		auto ssi = decodeServerListValue(serverList.get()[i].value);
		results.emplace_back(ssi, id_data[ssi.locality.processId()].processClass);
	}

	return results;
}

// Create a transaction reading the value of `wigglingStorageServerKey` and update it to the next Process ID according
// to a sorted PID set maintained by the data distributor. If now no storage server exists, the new Process ID is 0.
ACTOR Future<Void> updateNextWigglingStoragePID(DDTeamCollection* teamCollection) {
	state ReadYourWritesTransaction tr(teamCollection->cx);
	state Value writeValue;
	state const Key writeKey =
	    wigglingStorageServerKey.withSuffix(teamCollection->primary ? "/primary"_sr : "/remote"_sr);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			Optional<Value> value = wait(tr.get(writeKey));
			if (teamCollection->pid2server_info.empty()) {
				writeValue = LiteralStringRef("");
			} else {
				Value pid = teamCollection->pid2server_info.begin()->first;
				if (value.present()) {
					auto nextIt = teamCollection->pid2server_info.upper_bound(value.get());
					if (nextIt == teamCollection->pid2server_info.end()) {
						writeValue = pid;
					} else {
						writeValue = nextIt->first;
					}
				} else {
					writeValue = pid;
				}
			}
			tr.set(writeKey, writeValue);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
	TraceEvent(SevDebug, "PerpetualNextWigglingStoragePID", teamCollection->distributorId)
	    .detail("Primary", teamCollection->primary)
	    .detail("WriteValue", writeValue);

	return Void();
}

// Iterate over each storage process to do storage wiggle. After initializing the first Process ID, it waits a signal
// from `perpetualStorageWiggler` indicating the wiggling of current process is finished. Then it writes the next
// Process ID to a system key: `wigglingStorageServerKey` to show the next process to wiggle.
ACTOR Future<Void> perpetualStorageWiggleIterator(AsyncVar<bool>* stopSignal,
                                                  FutureStream<Void> finishStorageWiggleSignal,
                                                  DDTeamCollection* teamCollection) {
	loop {
		choose {
			when(wait(stopSignal->onChange())) {}
			when(waitNext(finishStorageWiggleSignal)) {
				state bool takeRest = true; // delay to avoid delete and update ServerList too frequently
				while (takeRest) {
					wait(delayJittered(SERVER_KNOBS->PERPETUAL_WIGGLE_DELAY));
					// there must not have other teams to place wiggled data
					takeRest = teamCollection->server_info.size() <= teamCollection->configuration.storageTeamSize ||
					           teamCollection->machine_info.size() < teamCollection->configuration.storageTeamSize;
					teamCollection->doBuildTeams = true;
					if (takeRest &&
					    teamCollection->configuration.storageMigrationType == StorageMigrationType::GRADUAL) {
						TraceEvent(SevWarn, "PerpetualWiggleSleep", teamCollection->distributorId)
						    .suppressFor(SERVER_KNOBS->PERPETUAL_WIGGLE_DELAY * 4)
						    .detail("ServerSize", teamCollection->server_info.size())
						    .detail("MachineSize", teamCollection->machine_info.size())
						    .detail("StorageTeamSize", teamCollection->configuration.storageTeamSize);
					}
				}
				wait(updateNextWigglingStoragePID(teamCollection));
			}
		}
		if (stopSignal->get()) {
			break;
		}
	}

	return Void();
}

// Watch the value change of `wigglingStorageServerKey`.
// Return the watch future and the current value of `wigglingStorageServerKey`.
ACTOR Future<std::pair<Future<Void>, Value>> watchPerpetualStoragePIDChange(DDTeamCollection* self) {
	state ReadYourWritesTransaction tr(self->cx);
	state Future<Void> watchFuture;
	state Value ret;
	state const Key readKey = wigglingStorageServerKey.withSuffix(self->primary ? "/primary"_sr : "/remote"_sr);

	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			Optional<Value> value = wait(tr.get(readKey));
			if (value.present()) {
				ret = value.get();
			}
			watchFuture = tr.watch(readKey);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
	return std::make_pair(watchFuture, ret);
}

// periodically check whether the cluster is healthy if we continue perpetual wiggle
ACTOR Future<Void> clusterHealthCheckForPerpetualWiggle(DDTeamCollection* self, int* extraTeamCount) {
	state int pausePenalty = 1;
	loop {
		Promise<int> countp;
		self->getUnhealthyRelocationCount.send(countp);
		int count = wait(countp.getFuture());
		// pause wiggle when
		// a. DDQueue is busy with unhealthy relocation request
		// b. healthy teams are not enough
		// c. the overall disk space is not enough
		if (count >= SERVER_KNOBS->DD_STORAGE_WIGGLE_PAUSE_THRESHOLD || self->healthyTeamCount <= *extraTeamCount ||
		    self->bestTeamKeepStuckCount > SERVER_KNOBS->DD_STORAGE_WIGGLE_STUCK_THRESHOLD) {
			// if we pause wiggle not because the reason a, increase extraTeamCount. This helps avoid oscillation
			// between pause and non-pause status.
			if ((self->healthyTeamCount <= *extraTeamCount ||
			     self->bestTeamKeepStuckCount > SERVER_KNOBS->DD_STORAGE_WIGGLE_PAUSE_THRESHOLD) &&
			    !self->pauseWiggle->get()) {
				*extraTeamCount = std::min(*extraTeamCount + pausePenalty, (int)self->teams.size());
				pausePenalty = std::min(pausePenalty * 2, (int)self->teams.size());
			}
			self->pauseWiggle->set(true);
		} else {
			self->pauseWiggle->set(false);
		}
		wait(delay(SERVER_KNOBS->CHECK_TEAM_DELAY, TaskPriority::DataDistributionLow));
	}
}
// Watches the value (pid) change of \xff/storageWigglePID, and adds storage servers held on process of which the
// Process Id is “pid” into excludeServers which prevent recruiting the wiggling storage servers and let teamTracker
// start to move data off the affected teams. The wiggling process of current storage servers will be paused if the
// cluster is unhealthy and restarted once the cluster is healthy again.
ACTOR Future<Void> perpetualStorageWiggler(AsyncVar<bool>* stopSignal,
                                           PromiseStream<Void> finishStorageWiggleSignal,
                                           DDTeamCollection* self) {
	state Future<Void> watchFuture = Never();
	state Future<Void> moveFinishFuture = Never();
	state int extraTeamCount = 0;
	state Future<Void> ddQueueCheck = clusterHealthCheckForPerpetualWiggle(self, &extraTeamCount);
	state int movingCount = 0;
	state std::pair<Future<Void>, Value> res = wait(watchPerpetualStoragePIDChange(self));
	ASSERT(!self->wigglingPid.present()); // only single process wiggle is allowed
	self->wigglingPid = Optional<Key>(res.second);

	loop {
		if (self->wigglingPid.present()) {
			StringRef pid = self->wigglingPid.get();
			if (self->pauseWiggle->get()) {
				TEST(true); // paused because cluster is unhealthy
				moveFinishFuture = Never();
				self->includeStorageServersForWiggle();
				self->doBuildTeams = true;

				TraceEvent(self->configuration.storageMigrationType == StorageMigrationType::AGGRESSIVE ? SevInfo
				                                                                                        : SevWarn,
				           "PerpetualStorageWigglePause",
				           self->distributorId)
				    .detail("Primary", self->primary)
				    .detail("ProcessId", pid)
				    .detail("BestTeamKeepStuckCount", self->bestTeamKeepStuckCount)
				    .detail("ExtraHealthyTeamCount", extraTeamCount)
				    .detail("HealthyTeamCount", self->healthyTeamCount)
				    .detail("StorageCount", movingCount);
			} else {
				TEST(true); // start wiggling
				auto fv = self->excludeStorageServersForWiggle(pid);
				movingCount = fv.size();
				moveFinishFuture = waitForAll(fv);
				TraceEvent("PerpetualStorageWiggleStart", self->distributorId)
				    .detail("Primary", self->primary)
				    .detail("ProcessId", pid)
				    .detail("ExtraHealthyTeamCount", extraTeamCount)
				    .detail("HealthyTeamCount", self->healthyTeamCount)
				    .detail("StorageCount", movingCount);
			}
		}

		choose {
			when(wait(watchFuture)) {
				ASSERT(!self->wigglingPid.present()); // the previous wiggle must be finished
				watchFuture = Never();
				// read new pid and set the next watch Future
				wait(store(res, watchPerpetualStoragePIDChange(self)));
				self->wigglingPid = Optional<Key>(res.second);

				// random delay
				wait(delayJittered(5.0, TaskPriority::DataDistributionLow));
			}
			when(wait(moveFinishFuture)) {
				ASSERT(self->wigglingPid.present());
				StringRef pid = self->wigglingPid.get();
				TEST(pid != LiteralStringRef("")); // finish wiggling this process

				moveFinishFuture = Never();
				self->includeStorageServersForWiggle();
				TraceEvent("PerpetualStorageWiggleFinish", self->distributorId)
				    .detail("Primary", self->primary)
				    .detail("ProcessId", pid.toString())
				    .detail("StorageCount", movingCount);

				self->wigglingPid.reset();
				watchFuture = res.first;
				finishStorageWiggleSignal.send(Void());
				extraTeamCount = std::max(0, extraTeamCount - 1);
			}
			when(wait(ddQueueCheck || self->pauseWiggle->onChange() || stopSignal->onChange())) {}
		}

		if (stopSignal->get()) {
			break;
		}
	}

	if (self->wigglingPid.present()) {
		self->includeStorageServersForWiggle();
		TraceEvent("PerpetualStorageWiggleExitingPause", self->distributorId)
		    .detail("Primary", self->primary)
		    .detail("ProcessId", self->wigglingPid.get());
		self->wigglingPid.reset();
	}

	return Void();
}

// This coroutine sets a watch to monitor the value change of `perpetualStorageWiggleKey` which is controlled by command
// `configure perpetual_storage_wiggle=$value` if the value is 1, this actor start 2 actors,
// `perpetualStorageWiggleIterator` and `perpetualStorageWiggler`. Otherwise, it sends stop signal to them.
ACTOR Future<Void> monitorPerpetualStorageWiggle(DDTeamCollection* teamCollection) {
	state int speed = 0;
	state AsyncVar<bool> stopWiggleSignal(true);
	state PromiseStream<Void> finishStorageWiggleSignal;
	state SignalableActorCollection collection;
	teamCollection->pauseWiggle = makeReference<AsyncVar<bool>>(true);

	loop {
		state ReadYourWritesTransaction tr(teamCollection->cx);
		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				Optional<Standalone<StringRef>> value = wait(tr.get(perpetualStorageWiggleKey));

				if (value.present()) {
					speed = std::stoi(value.get().toString());
				}
				state Future<Void> watchFuture = tr.watch(perpetualStorageWiggleKey);
				wait(tr.commit());

				ASSERT(speed == 1 || speed == 0);
				if (speed == 1 && stopWiggleSignal.get()) { // avoid duplicated start
					stopWiggleSignal.set(false);
					collection.add(perpetualStorageWiggleIterator(
					    &stopWiggleSignal, finishStorageWiggleSignal.getFuture(), teamCollection));
					collection.add(
					    perpetualStorageWiggler(&stopWiggleSignal, finishStorageWiggleSignal, teamCollection));
					TraceEvent("PerpetualStorageWiggleOpen", teamCollection->distributorId)
					    .detail("Primary", teamCollection->primary);
				} else if (speed == 0) {
					if (!stopWiggleSignal.get()) {
						stopWiggleSignal.set(true);
						wait(collection.signalAndReset());
						teamCollection->pauseWiggle->set(true);
					}
					TraceEvent("PerpetualStorageWiggleClose", teamCollection->distributorId)
					    .detail("Primary", teamCollection->primary);
				}
				wait(watchFuture);
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}
}
// The serverList system keyspace keeps the StorageServerInterface for each serverID. Storage server's storeType
// and serverID are decided by the server's filename. By parsing storage server file's filename on each disk, process on
// each machine creates the TCServer with the correct serverID and StorageServerInterface.
ACTOR Future<Void> waitServerListChange(DDTeamCollection* self,
                                        FutureStream<Void> serverRemoved,
                                        const DDEnabledState* ddEnabledState) {
	state Future<Void> checkSignal = delay(SERVER_KNOBS->SERVER_LIST_DELAY, TaskPriority::DataDistributionLaunch);
	state Future<std::vector<std::pair<StorageServerInterface, ProcessClass>>> serverListAndProcessClasses = Never();
	state bool isFetchingResults = false;
	state Transaction tr(self->cx);
	loop {
		try {
			choose {
				when(wait(checkSignal)) {
					checkSignal = Never();
					isFetchingResults = true;
					serverListAndProcessClasses = getServerListAndProcessClasses(&tr);
				}
				when(std::vector<std::pair<StorageServerInterface, ProcessClass>> results =
				         wait(serverListAndProcessClasses)) {
					serverListAndProcessClasses = Never();
					isFetchingResults = false;

					for (int i = 0; i < results.size(); i++) {
						UID serverId = results[i].first.id();
						StorageServerInterface const& ssi = results[i].first;
						ProcessClass const& processClass = results[i].second;
						if (!self->shouldHandleServer(ssi)) {
							continue;
						} else if (self->server_and_tss_info.count(serverId)) {
							auto& serverInfo = self->server_and_tss_info[serverId];
							if (ssi.getValue.getEndpoint() != serverInfo->lastKnownInterface.getValue.getEndpoint() ||
							    processClass != serverInfo->lastKnownClass.classType()) {
								Promise<std::pair<StorageServerInterface, ProcessClass>> currentInterfaceChanged =
								    serverInfo->interfaceChanged;
								serverInfo->interfaceChanged =
								    Promise<std::pair<StorageServerInterface, ProcessClass>>();
								serverInfo->onInterfaceChanged =
								    Future<std::pair<StorageServerInterface, ProcessClass>>(
								        serverInfo->interfaceChanged.getFuture());
								currentInterfaceChanged.send(std::make_pair(ssi, processClass));
							}
						} else if (!self->recruitingIds.count(ssi.id())) {
							self->addServer(ssi,
							                processClass,
							                self->serverTrackerErrorOut,
							                tr.getReadVersion().get(),
							                ddEnabledState);
							if (!ssi.isTss()) {
								self->doBuildTeams = true;
							}
						}
					}

					tr = Transaction(self->cx);
					checkSignal = delay(SERVER_KNOBS->SERVER_LIST_DELAY, TaskPriority::DataDistributionLaunch);
				}
				when(waitNext(serverRemoved)) {
					if (isFetchingResults) {
						tr = Transaction(self->cx);
						serverListAndProcessClasses = getServerListAndProcessClasses(&tr);
					}
				}
			}
		} catch (Error& e) {
			wait(tr.onError(e));
			serverListAndProcessClasses = Never();
			isFetchingResults = false;
			checkSignal = Void();
		}
	}
}

ACTOR Future<Void> waitHealthyZoneChange(DDTeamCollection* self) {
	state ReadYourWritesTransaction tr(self->cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			Optional<Value> val = wait(tr.get(healthyZoneKey));
			state Future<Void> healthyZoneTimeout = Never();
			if (val.present()) {
				auto p = decodeHealthyZoneValue(val.get());
				if (p.first == ignoreSSFailuresZoneString) {
					// healthyZone is now overloaded for DD diabling purpose, which does not timeout
					TraceEvent("DataDistributionDisabledForStorageServerFailuresStart", self->distributorId).log();
					healthyZoneTimeout = Never();
				} else if (p.second > tr.getReadVersion().get()) {
					double timeoutSeconds =
					    (p.second - tr.getReadVersion().get()) / (double)SERVER_KNOBS->VERSIONS_PER_SECOND;
					healthyZoneTimeout = delay(timeoutSeconds, TaskPriority::DataDistribution);
					if (self->healthyZone.get() != p.first) {
						TraceEvent("MaintenanceZoneStart", self->distributorId)
						    .detail("ZoneID", printable(p.first))
						    .detail("EndVersion", p.second)
						    .detail("Duration", timeoutSeconds);
						self->healthyZone.set(p.first);
					}
				} else if (self->healthyZone.get().present()) {
					// maintenance hits timeout
					TraceEvent("MaintenanceZoneEndTimeout", self->distributorId).log();
					self->healthyZone.set(Optional<Key>());
				}
			} else if (self->healthyZone.get().present()) {
				// `healthyZone` has been cleared
				if (self->healthyZone.get().get() == ignoreSSFailuresZoneString) {
					TraceEvent("DataDistributionDisabledForStorageServerFailuresEnd", self->distributorId).log();
				} else {
					TraceEvent("MaintenanceZoneEndManualClear", self->distributorId).log();
				}
				self->healthyZone.set(Optional<Key>());
			}

			state Future<Void> watchFuture = tr.watch(healthyZoneKey);
			wait(tr.commit());
			wait(watchFuture || healthyZoneTimeout);
			tr.reset();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<Void> serverMetricsPolling(TCServerInfo* server) {
	state double lastUpdate = now();
	loop {
		wait(updateServerMetrics(server));
		wait(delayUntil(lastUpdate + SERVER_KNOBS->STORAGE_METRICS_POLLING_DELAY +
		                    SERVER_KNOBS->STORAGE_METRICS_RANDOM_DELAY * deterministicRandom()->random01(),
		                TaskPriority::DataDistributionLaunch));
		lastUpdate = now();
	}
}

// Set the server's storeType; Error is catched by the caller
ACTOR Future<Void> keyValueStoreTypeTracker(DDTeamCollection* self, TCServerInfo* server) {
	// Update server's storeType, especially when it was created
	state KeyValueStoreType type =
	    wait(brokenPromiseToNever(server->lastKnownInterface.getKeyValueStoreType.getReplyWithTaskID<KeyValueStoreType>(
	        TaskPriority::DataDistribution)));
	server->storeType = type;

	if (type != self->configuration.storageServerStoreType) {
		if (self->wrongStoreTypeRemover.isReady()) {
			self->wrongStoreTypeRemover = removeWrongStoreType(self);
			self->addActor.send(self->wrongStoreTypeRemover);
		}
	}

	return Never();
}

ACTOR Future<Void> waitForAllDataRemoved(Database cx, UID serverID, Version addedVersion, DDTeamCollection* teams) {
	state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(cx);
	loop {
		try {
			tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			Version ver = wait(tr->getReadVersion());

			// we cannot remove a server immediately after adding it, because a perfectly timed master recovery could
			// cause us to not store the mutations sent to the short lived storage server.
			if (ver > addedVersion + SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS) {
				bool canRemove = wait(canRemoveStorageServer(tr, serverID));
				// TraceEvent("WaitForAllDataRemoved")
				//     .detail("Server", serverID)
				//     .detail("CanRemove", canRemove)
				//     .detail("Shards", teams->shardsAffectedByTeamFailure->getNumberOfShards(serverID));
				ASSERT(teams->shardsAffectedByTeamFailure->getNumberOfShards(serverID) >= 0);
				if (canRemove && teams->shardsAffectedByTeamFailure->getNumberOfShards(serverID) == 0) {
					return Void();
				}
			}

			// Wait for any change to the serverKeys for this server
			wait(delay(SERVER_KNOBS->ALL_DATA_REMOVED_DELAY, TaskPriority::DataDistribution));
			tr->reset();
		} catch (Error& e) {
			wait(tr->onError(e));
		}
	}
}

ACTOR Future<Void> storageServerFailureTracker(DDTeamCollection* self,
                                               TCServerInfo* server,
                                               Database cx,
                                               ServerStatus* status,
                                               Version addedVersion) {
	state StorageServerInterface interf = server->lastKnownInterface;
	state int targetTeamNumPerServer =
	    (SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * (self->configuration.storageTeamSize + 1)) / 2;
	loop {
		state bool inHealthyZone = false; // healthChanged actor will be Never() if this flag is true
		if (self->healthyZone.get().present()) {
			if (interf.locality.zoneId() == self->healthyZone.get()) {
				status->isFailed = false;
				inHealthyZone = true;
			} else if (self->healthyZone.get().get() == ignoreSSFailuresZoneString) {
				// Ignore all SS failures
				status->isFailed = false;
				inHealthyZone = true;
				TraceEvent("SSFailureTracker", self->distributorId)
				    .suppressFor(1.0)
				    .detail("IgnoredFailure", "BeforeChooseWhen")
				    .detail("ServerID", interf.id())
				    .detail("Status", status->toString());
			}
		}

		if (!interf.isTss()) {
			if (self->server_status.get(interf.id()).initialized) {
				bool unhealthy = self->server_status.get(interf.id()).isUnhealthy();
				if (unhealthy && !status->isUnhealthy()) {
					self->unhealthyServers--;
				}
				if (!unhealthy && status->isUnhealthy()) {
					self->unhealthyServers++;
				}
			} else if (status->isUnhealthy()) {
				self->unhealthyServers++;
			}
		}

		self->server_status.set(interf.id(), *status);
		if (status->isFailed) {
			self->restartRecruiting.trigger();
		}

		Future<Void> healthChanged = Never();
		if (status->isFailed) {
			ASSERT(!inHealthyZone);
			healthChanged =
			    IFailureMonitor::failureMonitor().onStateEqual(interf.waitFailure.getEndpoint(), FailureStatus(false));
		} else if (!inHealthyZone) {
			healthChanged = waitFailureClientStrict(interf.waitFailure,
			                                        SERVER_KNOBS->DATA_DISTRIBUTION_FAILURE_REACTION_TIME,
			                                        TaskPriority::DataDistribution);
		}
		choose {
			when(wait(healthChanged)) {
				status->isFailed = !status->isFailed;
				if (!status->isFailed && !server->lastKnownInterface.isTss() &&
				    (server->teams.size() < targetTeamNumPerServer || self->lastBuildTeamsFailed)) {
					self->doBuildTeams = true;
				}
				if (status->isFailed && self->healthyZone.get().present()) {
					if (self->healthyZone.get().get() == ignoreSSFailuresZoneString) {
						// Ignore the failed storage server
						TraceEvent("SSFailureTracker", self->distributorId)
						    .detail("IgnoredFailure", "InsideChooseWhen")
						    .detail("ServerID", interf.id())
						    .detail("Status", status->toString());
						status->isFailed = false;
					} else if (self->clearHealthyZoneFuture.isReady()) {
						self->clearHealthyZoneFuture = clearHealthyZone(self->cx);
						TraceEvent("MaintenanceZoneCleared", self->distributorId).log();
						self->healthyZone.set(Optional<Key>());
					}
				}

				// TraceEvent("StatusMapChange", self->distributorId)
				//     .detail("ServerID", interf.id())
				//     .detail("Status", status->toString())
				//     .detail("Available",
				//             IFailureMonitor::failureMonitor().getState(interf.waitFailure.getEndpoint()).isAvailable());
			}
			when(wait(status->isUnhealthy() ? waitForAllDataRemoved(cx, interf.id(), addedVersion, self) : Never())) {
				break;
			}
			when(wait(self->healthyZone.onChange())) {}
		}
	}

	return Void(); // Don't ignore failures
}

// Check the status of a storage server.
// Apply all requirements to the server and mark it as excluded if it fails to satisfies these requirements
ACTOR Future<Void> storageServerTracker(
    DDTeamCollection* self,
    Database cx,
    TCServerInfo* server, // This actor is owned by this TCServerInfo, point to server_info[id]
    Promise<Void> errorOut,
    Version addedVersion,
    const DDEnabledState* ddEnabledState,
    bool isTss) {

	state Future<Void> failureTracker;
	state ServerStatus status(false, false, false, server->lastKnownInterface.locality);
	state bool lastIsUnhealthy = false;
	state Future<Void> metricsTracker = serverMetricsPolling(server);

	state Future<std::pair<StorageServerInterface, ProcessClass>> interfaceChanged = server->onInterfaceChanged;

	state Future<Void> storeTypeTracker = (isTss) ? Never() : keyValueStoreTypeTracker(self, server);
	state bool hasWrongDC = !isCorrectDC(self, server);
	state bool hasInvalidLocality =
	    !self->isValidLocality(self->configuration.storagePolicy, server->lastKnownInterface.locality);
	state int targetTeamNumPerServer =
	    (SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * (self->configuration.storageTeamSize + 1)) / 2;

	try {
		loop {
			status.isUndesired = !self->disableFailingLaggingServers.get() && server->ssVersionTooFarBehind.get();
			status.isWrongConfiguration = false;
			status.isWiggling = false;
			hasWrongDC = !isCorrectDC(self, server);
			hasInvalidLocality =
			    !self->isValidLocality(self->configuration.storagePolicy, server->lastKnownInterface.locality);

			// If there is any other server on this exact NetworkAddress, this server is undesired and will eventually
			// be eliminated. This samAddress checking must be redo whenever the server's state (e.g., storeType,
			// dcLocation, interface) is changed.
			state std::vector<Future<Void>> otherChanges;
			std::vector<Promise<Void>> wakeUpTrackers;
			for (const auto& i : self->server_and_tss_info) {
				if (i.second.getPtr() != server &&
				    i.second->lastKnownInterface.address() == server->lastKnownInterface.address()) {
					auto& statusInfo = self->server_status.get(i.first);
					TraceEvent("SameAddress", self->distributorId)
					    .detail("Failed", statusInfo.isFailed)
					    .detail("Undesired", statusInfo.isUndesired)
					    .detail("Server", server->id)
					    .detail("OtherServer", i.second->id)
					    .detail("Address", server->lastKnownInterface.address())
					    .detail("NumShards", self->shardsAffectedByTeamFailure->getNumberOfShards(server->id))
					    .detail("OtherNumShards", self->shardsAffectedByTeamFailure->getNumberOfShards(i.second->id))
					    .detail("OtherHealthy", !self->server_status.get(i.second->id).isUnhealthy());
					// wait for the server's ip to be changed
					otherChanges.push_back(self->server_status.onChange(i.second->id));
					if (!self->server_status.get(i.second->id).isUnhealthy()) {
						if (self->shardsAffectedByTeamFailure->getNumberOfShards(i.second->id) >=
						    self->shardsAffectedByTeamFailure->getNumberOfShards(server->id)) {
							TraceEvent(SevWarn, "UndesiredStorageServer", self->distributorId)
							    .detail("Server", server->id)
							    .detail("Address", server->lastKnownInterface.address())
							    .detail("OtherServer", i.second->id)
							    .detail("NumShards", self->shardsAffectedByTeamFailure->getNumberOfShards(server->id))
							    .detail("OtherNumShards",
							            self->shardsAffectedByTeamFailure->getNumberOfShards(i.second->id));

							status.isUndesired = true;
						} else
							wakeUpTrackers.push_back(i.second->wakeUpTracker);
					}
				}
			}

			for (auto& p : wakeUpTrackers) {
				if (!p.isSet())
					p.send(Void());
			}

			if (server->lastKnownClass.machineClassFitness(ProcessClass::Storage) > ProcessClass::UnsetFit) {
				// NOTE: Should not use self->healthyTeamCount > 0 in if statement, which will cause status bouncing
				// between healthy and unhealthy and result in OOM (See PR#2228).

				if (self->optimalTeamCount > 0) {
					TraceEvent(SevWarn, "UndesiredStorageServer", self->distributorId)
					    .detail("Server", server->id)
					    .detail("OptimalTeamCount", self->optimalTeamCount)
					    .detail("Fitness", server->lastKnownClass.machineClassFitness(ProcessClass::Storage));
					status.isUndesired = true;
				}
				otherChanges.push_back(self->zeroOptimalTeams.onChange());
			}

			// If this storage server has the wrong key-value store type, then mark it undesired so it will be replaced
			// with a server having the correct type
			if (hasWrongDC || hasInvalidLocality) {
				TraceEvent(SevWarn, "UndesiredDCOrLocality", self->distributorId)
				    .detail("Server", server->id)
				    .detail("WrongDC", hasWrongDC)
				    .detail("InvalidLocality", hasInvalidLocality);
				status.isUndesired = true;
				status.isWrongConfiguration = true;
			}
			if (server->wrongStoreTypeToRemove.get()) {
				TraceEvent(SevWarn, "WrongStoreTypeToRemove", self->distributorId)
				    .detail("Server", server->id)
				    .detail("StoreType", "?");
				status.isUndesired = true;
				status.isWrongConfiguration = true;
			}

			// An invalid wiggle server should set itself the right status. Otherwise, it cannot be re-included by
			// wiggler.
			auto invalidWiggleServer =
			    [](const AddressExclusion& addr, const DDTeamCollection* tc, const TCServerInfo* server) {
				    return server->lastKnownInterface.locality.processId() != tc->wigglingPid;
			    };
			// If the storage server is in the excluded servers list, it is undesired
			NetworkAddress a = server->lastKnownInterface.address();
			AddressExclusion worstAddr(a.ip, a.port);
			DDTeamCollection::Status worstStatus = self->excludedServers.get(worstAddr);

			if (worstStatus == DDTeamCollection::Status::WIGGLING && invalidWiggleServer(worstAddr, self, server)) {
				TraceEvent(SevInfo, "InvalidWiggleServer", self->distributorId)
				    .detail("Address", worstAddr.toString())
				    .detail("ProcessId", server->lastKnownInterface.locality.processId())
				    .detail("ValidWigglingId", self->wigglingPid.present());
				self->excludedServers.set(worstAddr, DDTeamCollection::Status::NONE);
				worstStatus = DDTeamCollection::Status::NONE;
			}
			otherChanges.push_back(self->excludedServers.onChange(worstAddr));

			for (int i = 0; i < 3; i++) {
				if (i > 0 && !server->lastKnownInterface.secondaryAddress().present()) {
					break;
				}
				AddressExclusion testAddr;
				if (i == 0)
					testAddr = AddressExclusion(a.ip);
				else if (i == 1)
					testAddr = AddressExclusion(server->lastKnownInterface.secondaryAddress().get().ip,
					                            server->lastKnownInterface.secondaryAddress().get().port);
				else if (i == 2)
					testAddr = AddressExclusion(server->lastKnownInterface.secondaryAddress().get().ip);
				DDTeamCollection::Status testStatus = self->excludedServers.get(testAddr);

				if (testStatus == DDTeamCollection::Status::WIGGLING && invalidWiggleServer(testAddr, self, server)) {
					TraceEvent(SevInfo, "InvalidWiggleServer", self->distributorId)
					    .detail("Address", testAddr.toString())
					    .detail("ProcessId", server->lastKnownInterface.locality.processId())
					    .detail("ValidWigglingId", self->wigglingPid.present());
					self->excludedServers.set(testAddr, DDTeamCollection::Status::NONE);
					testStatus = DDTeamCollection::Status::NONE;
				}

				if (testStatus > worstStatus) {
					worstStatus = testStatus;
					worstAddr = testAddr;
				}
				otherChanges.push_back(self->excludedServers.onChange(testAddr));
			}

			if (worstStatus != DDTeamCollection::Status::NONE) {
				TraceEvent(SevWarn, "UndesiredStorageServer", self->distributorId)
				    .detail("Server", server->id)
				    .detail("Excluded", worstAddr.toString());
				status.isUndesired = true;
				status.isWrongConfiguration = true;

				if (worstStatus == DDTeamCollection::Status::WIGGLING && !isTss) {
					status.isWiggling = true;
					TraceEvent("PerpetualWigglingStorageServer", self->distributorId)
					    .detail("Primary", self->primary)
					    .detail("Server", server->id)
					    .detail("ProcessId", server->lastKnownInterface.locality.processId())
					    .detail("Address", worstAddr.toString());
				} else if (worstStatus == DDTeamCollection::Status::FAILED && !isTss) {
					TraceEvent(SevWarn, "FailedServerRemoveKeys", self->distributorId)
					    .detail("Server", server->id)
					    .detail("Excluded", worstAddr.toString());
					wait(delay(0.0)); // Do not throw an error while still inside trackExcludedServers
					while (!ddEnabledState->isDDEnabled()) {
						wait(delay(1.0));
					}
					if (self->removeFailedServer.canBeSet()) {
						self->removeFailedServer.send(server->id);
					}
					throw movekeys_conflict();
				}
			}

			failureTracker = storageServerFailureTracker(self, server, cx, &status, addedVersion);
			// We need to recruit new storage servers if the key value store type has changed
			if (hasWrongDC || hasInvalidLocality || server->wrongStoreTypeToRemove.get()) {
				self->restartRecruiting.trigger();
			}

			if (lastIsUnhealthy && !status.isUnhealthy() && !isTss &&
			    (server->teams.size() < targetTeamNumPerServer || self->lastBuildTeamsFailed)) {
				self->doBuildTeams = true;
				self->restartTeamBuilder.trigger(); // This does not trigger building teams if there exist healthy teams
			}
			lastIsUnhealthy = status.isUnhealthy();

			state bool recordTeamCollectionInfo = false;
			choose {
				when(wait(failureTracker || server->onTSSPairRemoved || server->killTss.getFuture())) {
					// The server is failed AND all data has been removed from it, so permanently remove it.
					TraceEvent("StatusMapChange", self->distributorId)
					    .detail("ServerID", server->id)
					    .detail("Status", "Removing");

					if (server->updated.canBeSet()) {
						server->updated.send(Void());
					}

					// Remove server from FF/serverList
					wait(removeStorageServer(
					    cx, server->id, server->lastKnownInterface.tssPairID, self->lock, ddEnabledState));

					TraceEvent("StatusMapChange", self->distributorId)
					    .detail("ServerID", server->id)
					    .detail("Status", "Removed");
					// Sets removeSignal (alerting dataDistributionTeamCollection to remove the storage server from its
					// own data structures)
					server->removed.send(Void());
					if (isTss) {
						self->removedTSS.send(server->id);
					} else {
						self->removedServers.send(server->id);
					}
					return Void();
				}
				when(std::pair<StorageServerInterface, ProcessClass> newInterface = wait(interfaceChanged)) {
					bool restartRecruiting = newInterface.first.waitFailure.getEndpoint().getPrimaryAddress() !=
					                         server->lastKnownInterface.waitFailure.getEndpoint().getPrimaryAddress();
					bool localityChanged = server->lastKnownInterface.locality != newInterface.first.locality;
					bool machineLocalityChanged = server->lastKnownInterface.locality.zoneId().get() !=
					                              newInterface.first.locality.zoneId().get();
					bool processIdChanged = server->lastKnownInterface.locality.processId().get() !=
					                        newInterface.first.locality.processId().get();
					TraceEvent("StorageServerInterfaceChanged", self->distributorId)
					    .detail("ServerID", server->id)
					    .detail("NewWaitFailureToken", newInterface.first.waitFailure.getEndpoint().token)
					    .detail("OldWaitFailureToken", server->lastKnownInterface.waitFailure.getEndpoint().token)
					    .detail("LocalityChanged", localityChanged)
					    .detail("ProcessIdChanged", processIdChanged)
					    .detail("MachineLocalityChanged", machineLocalityChanged);

					server->lastKnownInterface = newInterface.first;
					server->lastKnownClass = newInterface.second;
					if (localityChanged && !isTss) {
						TEST(true); // Server locality changed

						// The locality change of a server will affect machine teams related to the server if
						// the server's machine locality is changed
						if (machineLocalityChanged) {
							// First handle the impact on the machine of the server on the old locality
							Reference<TCMachineInfo> machine = server->machine;
							ASSERT(machine->serversOnMachine.size() >= 1);
							if (machine->serversOnMachine.size() == 1) {
								// When server is the last server on the machine,
								// remove the machine and the related machine team
								self->removeMachine(machine);
								server->machine = Reference<TCMachineInfo>();
							} else {
								// we remove the server from the machine, and
								// update locality entry for the machine and the global machineLocalityMap
								int serverIndex = -1;
								for (int i = 0; i < machine->serversOnMachine.size(); ++i) {
									if (machine->serversOnMachine[i].getPtr() == server) {
										// NOTE: now the machine's locality is wrong. Need update it whenever uses it.
										serverIndex = i;
										machine->serversOnMachine[i] = machine->serversOnMachine.back();
										machine->serversOnMachine.pop_back();
										break; // Invariant: server only appear on the machine once
									}
								}
								ASSERT(serverIndex != -1);
								// NOTE: we do not update the machine's locality map even when
								// its representative server is changed.
							}

							// Second handle the impact on the destination machine where the server's new locality is;
							// If the destination machine is new, create one; otherwise, add server to an existing one
							// Update server's machine reference to the destination machine
							Reference<TCMachineInfo> destMachine =
							    self->checkAndCreateMachine(self->server_info[server->id]);
							ASSERT(destMachine.isValid());
						}

						// update pid2server_info if the process id has changed
						if (processIdChanged) {
							self->pid2server_info[newInterface.first.locality.processId().get()].push_back(
							    self->server_info[server->id]);
							// delete the old one
							auto& old_infos =
							    self->pid2server_info[server->lastKnownInterface.locality.processId().get()];
							for (int i = 0; i < old_infos.size(); ++i) {
								if (old_infos[i].getPtr() == server) {
									std::swap(old_infos[i--], old_infos.back());
									old_infos.pop_back();
								}
							}
						}
						// Ensure the server's server team belong to a machine team, and
						// Get the newBadTeams due to the locality change
						std::vector<Reference<TCTeamInfo>> newBadTeams;
						for (auto& serverTeam : server->teams) {
							if (!self->satisfiesPolicy(serverTeam->getServers())) {
								newBadTeams.push_back(serverTeam);
								continue;
							}
							if (machineLocalityChanged) {
								Reference<TCMachineTeamInfo> machineTeam = self->checkAndCreateMachineTeam(serverTeam);
								ASSERT(machineTeam.isValid());
								serverTeam->machineTeam = machineTeam;
							}
						}

						server->inDesiredDC =
						    (self->includedDCs.empty() ||
						     std::find(self->includedDCs.begin(),
						               self->includedDCs.end(),
						               server->lastKnownInterface.locality.dcId()) != self->includedDCs.end());
						self->resetLocalitySet();

						bool addedNewBadTeam = false;
						for (auto it : newBadTeams) {
							if (self->removeTeam(it)) {
								self->addTeam(it->getServers(), true);
								addedNewBadTeam = true;
							}
						}
						if (addedNewBadTeam && self->badTeamRemover.isReady()) {
							TEST(true); // Server locality change created bad teams
							self->doBuildTeams = true;
							self->badTeamRemover = removeBadTeams(self);
							self->addActor.send(self->badTeamRemover);
							// The team number changes, so we need to update the team number info
							// self->traceTeamCollectionInfo();
							recordTeamCollectionInfo = true;
						}
						// The locality change of the server will invalid the server's old teams,
						// so we need to rebuild teams for the server
						self->doBuildTeams = true;
					}

					interfaceChanged = server->onInterfaceChanged;
					// Old failureTracker for the old interface will be actorCancelled since the handler of the old
					// actor now points to the new failure monitor actor.
					status = ServerStatus(
					    status.isFailed, status.isUndesired, status.isWiggling, server->lastKnownInterface.locality);

					// self->traceTeamCollectionInfo();
					recordTeamCollectionInfo = true;
					// Restart the storeTracker for the new interface. This will cancel the previous
					// keyValueStoreTypeTracker
					storeTypeTracker = (isTss) ? Never() : keyValueStoreTypeTracker(self, server);
					hasWrongDC = !isCorrectDC(self, server);
					hasInvalidLocality =
					    !self->isValidLocality(self->configuration.storagePolicy, server->lastKnownInterface.locality);
					self->restartTeamBuilder.trigger();

					if (restartRecruiting)
						self->restartRecruiting.trigger();
				}
				when(wait(otherChanges.empty() ? Never() : quorum(otherChanges, 1))) {
					TraceEvent("SameAddressChangedStatus", self->distributorId).detail("ServerID", server->id);
				}
				when(wait(server->wrongStoreTypeToRemove.onChange())) {
					TraceEvent("UndesiredStorageServerTriggered", self->distributorId)
					    .detail("Server", server->id)
					    .detail("StoreType", server->storeType)
					    .detail("ConfigStoreType", self->configuration.storageServerStoreType)
					    .detail("WrongStoreTypeRemoved", server->wrongStoreTypeToRemove.get());
				}
				when(wait(server->wakeUpTracker.getFuture())) { server->wakeUpTracker = Promise<Void>(); }
				when(wait(storeTypeTracker)) {}
				when(wait(server->ssVersionTooFarBehind.onChange())) {}
				when(wait(self->disableFailingLaggingServers.onChange())) {}
			}

			if (recordTeamCollectionInfo) {
				self->traceTeamCollectionInfo();
			}
		}
	} catch (Error& e) {
		state Error err = e;
		TraceEvent("StorageServerTrackerCancelled", self->distributorId)
		    .suppressFor(1.0)
		    .detail("Primary", self->primary)
		    .detail("Server", server->id)
		    .error(e, /*includeCancelled*/ true);
		if (e.code() != error_code_actor_cancelled && errorOut.canBeSet()) {
			errorOut.sendError(e);
			wait(delay(0)); // Check for cancellation, since errorOut.sendError(e) could delete self
		}
		throw err;
	}
}

// Monitor whether or not storage servers are being recruited.  If so, then a database cannot be considered quiet
ACTOR Future<Void> monitorStorageServerRecruitment(DDTeamCollection* self) {
	state bool recruiting = false;
	state bool lastIsTss = false;
	TraceEvent("StorageServerRecruitment", self->distributorId)
	    .detail("State", "Idle")
	    .trackLatest("StorageServerRecruitment_" + self->distributorId.toString());
	loop {
		if (!recruiting) {
			while (self->recruitingStream.get() == 0) {
				wait(self->recruitingStream.onChange());
			}
			TraceEvent("StorageServerRecruitment", self->distributorId)
			    .detail("State", "Recruiting")
			    .detail("IsTSS", self->isTssRecruiting ? "True" : "False")
			    .trackLatest("StorageServerRecruitment_" + self->distributorId.toString());
			recruiting = true;
			lastIsTss = self->isTssRecruiting;
		} else {
			loop {
				choose {
					when(wait(self->recruitingStream.onChange())) {
						if (lastIsTss != self->isTssRecruiting) {
							TraceEvent("StorageServerRecruitment", self->distributorId)
							    .detail("State", "Recruiting")
							    .detail("IsTSS", self->isTssRecruiting ? "True" : "False")
							    .trackLatest("StorageServerRecruitment_" + self->distributorId.toString());
							lastIsTss = self->isTssRecruiting;
						}
					}
					when(wait(self->recruitingStream.get() == 0
					              ? delay(SERVER_KNOBS->RECRUITMENT_IDLE_DELAY, TaskPriority::DataDistribution)
					              : Future<Void>(Never()))) {
						break;
					}
				}
			}
			TraceEvent("StorageServerRecruitment", self->distributorId)
			    .detail("State", "Idle")
			    .trackLatest("StorageServerRecruitment_" + self->distributorId.toString());
			recruiting = false;
		}
	}
}

ACTOR Future<Void> checkAndRemoveInvalidLocalityAddr(DDTeamCollection* self) {
	state double start = now();
	state bool hasCorrectedLocality = false;

	loop {
		try {
			wait(delay(SERVER_KNOBS->DD_CHECK_INVALID_LOCALITY_DELAY, TaskPriority::DataDistribution));

			// Because worker's processId can be changed when its locality is changed, we cannot watch on the old
			// processId; This actor is inactive most time, so iterating all workers incurs little performance overhead.
			state std::vector<ProcessData> workers = wait(getWorkers(self->cx));
			state std::set<AddressExclusion> existingAddrs;
			for (int i = 0; i < workers.size(); i++) {
				const ProcessData& workerData = workers[i];
				AddressExclusion addr(workerData.address.ip, workerData.address.port);
				existingAddrs.insert(addr);
				if (self->invalidLocalityAddr.count(addr) &&
				    self->isValidLocality(self->configuration.storagePolicy, workerData.locality)) {
					// The locality info on the addr has been corrected
					self->invalidLocalityAddr.erase(addr);
					hasCorrectedLocality = true;
					TraceEvent("InvalidLocalityCorrected").detail("Addr", addr.toString());
				}
			}

			wait(yield(TaskPriority::DataDistribution));

			// In case system operator permanently excludes workers on the address with invalid locality
			for (auto addr = self->invalidLocalityAddr.begin(); addr != self->invalidLocalityAddr.end();) {
				if (!existingAddrs.count(*addr)) {
					// The address no longer has a worker
					addr = self->invalidLocalityAddr.erase(addr);
					hasCorrectedLocality = true;
					TraceEvent("InvalidLocalityNoLongerExists").detail("Addr", addr->toString());
				} else {
					++addr;
				}
			}

			if (hasCorrectedLocality) {
				// Recruit on address who locality has been corrected
				self->restartRecruiting.trigger();
				hasCorrectedLocality = false;
			}

			if (self->invalidLocalityAddr.empty()) {
				break;
			}

			if (now() - start > 300) { // Report warning if invalid locality is not corrected within 300 seconds
				// The incorrect locality info has not been properly corrected in a reasonable time
				TraceEvent(SevWarn, "PersistentInvalidLocality").detail("Addresses", self->invalidLocalityAddr.size());
				start = now();
			}
		} catch (Error& e) {
			TraceEvent("CheckAndRemoveInvalidLocalityAddrRetry", self->distributorId).detail("Error", e.what());
		}
	}

	return Void();
}

int numExistingSSOnAddr(DDTeamCollection* self, const AddressExclusion& addr) {
	int numExistingSS = 0;
	for (auto& server : self->server_and_tss_info) {
		const NetworkAddress& netAddr = server.second->lastKnownInterface.stableAddress();
		AddressExclusion usedAddr(netAddr.ip, netAddr.port);
		if (usedAddr == addr) {
			++numExistingSS;
		}
	}

	return numExistingSS;
}

// All state that represents an ongoing tss pair recruitment
struct TSSPairState : ReferenceCounted<TSSPairState>, NonCopyable {
	Promise<Optional<std::pair<UID, Version>>>
	    ssPairInfo; // if set, for ss to pass its id to tss pair once it is successfully recruited
	Promise<bool> tssPairDone; // if set, for tss to pass ss that it was successfully recruited
	Promise<Void> complete;

	Optional<Key> dcId; // dc
	Optional<Key> dataHallId; // data hall

	bool active;

	TSSPairState() : active(false) {}

	TSSPairState(const LocalityData& locality)
	  : dcId(locality.dcId()), dataHallId(locality.dataHallId()), active(true) {}

	bool inDataZone(const LocalityData& locality) {
		return locality.dcId() == dcId && locality.dataHallId() == dataHallId;
	}

	void cancel() {
		// only cancel if both haven't been set, otherwise one half of pair could think it was successful but the other
		// half would think it failed
		if (active && ssPairInfo.canBeSet() && tssPairDone.canBeSet()) {
			ssPairInfo.send(Optional<std::pair<UID, Version>>());
			// callback of ssPairInfo could have cancelled tssPairDone already, so double check before cancelling
			if (tssPairDone.canBeSet()) {
				tssPairDone.send(false);
			}
			if (complete.canBeSet()) {
				complete.send(Void());
			}
		}
	}

	bool tssRecruitSuccess() {
		if (active && tssPairDone.canBeSet()) {
			tssPairDone.send(true);
			return true;
		}
		return false;
	}

	bool tssRecruitFailed() {
		if (active && tssPairDone.canBeSet()) {
			tssPairDone.send(false);
			return true;
		}
		return false;
	}

	bool ssRecruitSuccess(std::pair<UID, Version> ssInfo) {
		if (active && ssPairInfo.canBeSet()) {
			ssPairInfo.send(Optional<std::pair<UID, Version>>(ssInfo));
			return true;
		}
		return false;
	}

	bool ssRecruitFailed() {
		if (active && ssPairInfo.canBeSet()) {
			ssPairInfo.send(Optional<std::pair<UID, Version>>());
			return true;
		}
		return false;
	}

	bool markComplete() {
		if (active && complete.canBeSet()) {
			complete.send(Void());
			return true;
		}
		return false;
	}

	Future<Optional<std::pair<UID, Version>>> waitOnSS() { return ssPairInfo.getFuture(); }

	Future<bool> waitOnTSS() { return tssPairDone.getFuture(); }

	Future<Void> waitComplete() { return complete.getFuture(); }
};

ACTOR Future<Void> initializeStorage(DDTeamCollection* self,
                                     RecruitStorageReply candidateWorker,
                                     const DDEnabledState* ddEnabledState,
                                     bool recruitTss,
                                     Reference<TSSPairState> tssState) {
	// SOMEDAY: Cluster controller waits for availability, retry quickly if a server's Locality changes
	self->recruitingStream.set(self->recruitingStream.get() + 1);

	const NetworkAddress& netAddr = candidateWorker.worker.stableAddress();
	AddressExclusion workerAddr(netAddr.ip, netAddr.port);
	if (numExistingSSOnAddr(self, workerAddr) <= 2 &&
	    self->recruitingLocalities.find(candidateWorker.worker.stableAddress()) == self->recruitingLocalities.end()) {
		// Only allow at most 2 storage servers on an address, because
		// too many storage server on the same address (i.e., process) can cause OOM.
		// Ask the candidateWorker to initialize a SS only if the worker does not have a pending request
		state UID interfaceId = deterministicRandom()->randomUniqueID();

		state InitializeStorageRequest isr;
		isr.storeType =
		    recruitTss ? self->configuration.testingStorageServerStoreType : self->configuration.storageServerStoreType;
		isr.seedTag = invalidTag;
		isr.reqId = deterministicRandom()->randomUniqueID();
		isr.interfaceId = interfaceId;

		self->recruitingIds.insert(interfaceId);
		self->recruitingLocalities.insert(candidateWorker.worker.stableAddress());

		// if tss, wait for pair ss to finish and add its id to isr. If pair fails, don't recruit tss
		state bool doRecruit = true;
		if (recruitTss) {
			TraceEvent("TSS_Recruit", self->distributorId)
			    .detail("TSSID", interfaceId)
			    .detail("Stage", "TSSWaitingPair")
			    .detail("Addr", candidateWorker.worker.address())
			    .detail("Locality", candidateWorker.worker.locality.toString());

			Optional<std::pair<UID, Version>> ssPairInfoResult = wait(tssState->waitOnSS());
			if (ssPairInfoResult.present()) {
				isr.tssPairIDAndVersion = ssPairInfoResult.get();

				TraceEvent("TSS_Recruit", self->distributorId)
				    .detail("SSID", ssPairInfoResult.get().first)
				    .detail("TSSID", interfaceId)
				    .detail("Stage", "TSSWaitingPair")
				    .detail("Addr", candidateWorker.worker.address())
				    .detail("Version", ssPairInfoResult.get().second)
				    .detail("Locality", candidateWorker.worker.locality.toString());
			} else {
				doRecruit = false;

				TraceEvent(SevWarnAlways, "TSS_RecruitError", self->distributorId)
				    .detail("TSSID", interfaceId)
				    .detail("Reason", "SS recruitment failed for some reason")
				    .detail("Addr", candidateWorker.worker.address())
				    .detail("Locality", candidateWorker.worker.locality.toString());
			}
		}

		TraceEvent("DDRecruiting")
		    .detail("Primary", self->primary)
		    .detail("State", "Sending request to worker")
		    .detail("WorkerID", candidateWorker.worker.id())
		    .detail("WorkerLocality", candidateWorker.worker.locality.toString())
		    .detail("Interf", interfaceId)
		    .detail("Addr", candidateWorker.worker.address())
		    .detail("TSS", recruitTss ? "true" : "false")
		    .detail("RecruitingStream", self->recruitingStream.get());

		Future<ErrorOr<InitializeStorageReply>> fRecruit =
		    doRecruit ? candidateWorker.worker.storage.tryGetReply(isr, TaskPriority::DataDistribution)
		              : Future<ErrorOr<InitializeStorageReply>>(ErrorOr<InitializeStorageReply>(recruitment_failed()));

		state ErrorOr<InitializeStorageReply> newServer = wait(fRecruit);

		if (doRecruit && newServer.isError()) {
			TraceEvent(SevWarn, "DDRecruitmentError").error(newServer.getError());
			if (!newServer.isError(error_code_recruitment_failed) &&
			    !newServer.isError(error_code_request_maybe_delivered)) {
				tssState->markComplete();
				throw newServer.getError();
			}
			wait(delay(SERVER_KNOBS->STORAGE_RECRUITMENT_DELAY, TaskPriority::DataDistribution));
		}

		if (!recruitTss && newServer.present() &&
		    tssState->ssRecruitSuccess(std::pair(interfaceId, newServer.get().addedVersion))) {
			// SS has a tss pair. send it this id, but try to wait for add server until tss is recruited

			TraceEvent("TSS_Recruit", self->distributorId)
			    .detail("SSID", interfaceId)
			    .detail("Stage", "SSSignaling")
			    .detail("Addr", candidateWorker.worker.address())
			    .detail("Locality", candidateWorker.worker.locality.toString());

			// wait for timeout, but eventually move on if no TSS pair recruited
			Optional<bool> tssSuccessful = wait(timeout(tssState->waitOnTSS(), SERVER_KNOBS->TSS_RECRUITMENT_TIMEOUT));

			if (tssSuccessful.present() && tssSuccessful.get()) {
				TraceEvent("TSS_Recruit", self->distributorId)
				    .detail("SSID", interfaceId)
				    .detail("Stage", "SSGotPair")
				    .detail("Addr", candidateWorker.worker.address())
				    .detail("Locality", candidateWorker.worker.locality.toString());
			} else {
				TraceEvent(SevWarn, "TSS_RecruitError", self->distributorId)
				    .detail("SSID", interfaceId)
				    .detail("Reason",
				            tssSuccessful.present() ? "TSS recruitment failed for some reason"
				                                    : "TSS recruitment timed out")
				    .detail("Addr", candidateWorker.worker.address())
				    .detail("Locality", candidateWorker.worker.locality.toString());
			}
		}

		self->recruitingIds.erase(interfaceId);
		self->recruitingLocalities.erase(candidateWorker.worker.stableAddress());

		TraceEvent("DDRecruiting")
		    .detail("Primary", self->primary)
		    .detail("State", "Finished request")
		    .detail("WorkerID", candidateWorker.worker.id())
		    .detail("WorkerLocality", candidateWorker.worker.locality.toString())
		    .detail("Interf", interfaceId)
		    .detail("Addr", candidateWorker.worker.address())
		    .detail("RecruitingStream", self->recruitingStream.get());

		if (newServer.present()) {
			UID id = newServer.get().interf.id();
			if (!self->server_and_tss_info.count(id)) {
				if (!recruitTss || tssState->tssRecruitSuccess()) {
					self->addServer(newServer.get().interf,
					                candidateWorker.processClass,
					                self->serverTrackerErrorOut,
					                newServer.get().addedVersion,
					                ddEnabledState);
					// signal all done after adding tss to tracking info
					tssState->markComplete();
				}
			} else {
				TraceEvent(SevWarn, "DDRecruitmentError")
				    .detail("Reason", "Server ID already recruited")
				    .detail("ServerID", id);
			}
			if (!recruitTss) {
				self->doBuildTeams = true;
			}
		}
	}

	// SS and/or TSS recruitment failed at this point, update tssState
	if (recruitTss && tssState->tssRecruitFailed()) {
		tssState->markComplete();
		TEST(true); // TSS recruitment failed for some reason
	}
	if (!recruitTss && tssState->ssRecruitFailed()) {
		TEST(true); // SS with pair TSS recruitment failed for some reason
	}

	self->recruitingStream.set(self->recruitingStream.get() - 1);
	self->restartRecruiting.trigger();

	return Void();
}

ACTOR Future<Void> storageRecruiter(DDTeamCollection* self,
                                    Reference<IAsyncListener<RequestStream<RecruitStorageRequest>>> recruitStorage,
                                    const DDEnabledState* ddEnabledState) {
	state Future<RecruitStorageReply> fCandidateWorker;
	state RecruitStorageRequest lastRequest;
	state bool hasHealthyTeam;
	state std::map<AddressExclusion, int> numSSPerAddr;

	// tss-specific recruitment state
	state int32_t targetTSSInDC = 0;
	state int32_t tssToRecruit = 0;
	state int inProgressTSSCount = 0;
	state PromiseStream<Future<Void>> addTSSInProgress;
	state Future<Void> inProgressTSS =
	    actorCollection(addTSSInProgress.getFuture(), &inProgressTSSCount, nullptr, nullptr, nullptr);
	state Reference<TSSPairState> tssState = makeReference<TSSPairState>();
	state Future<Void> checkTss = self->initialFailureReactionDelay;
	state bool pendingTSSCheck = false;

	TraceEvent(SevDebug, "TSS_RecruitUpdated", self->distributorId).detail("Count", tssToRecruit);

	loop {
		try {
			// Divide TSS evenly in each DC if there are multiple
			// TODO would it be better to put all of them in primary DC?
			targetTSSInDC = self->configuration.desiredTSSCount;
			if (self->configuration.usableRegions > 1) {
				targetTSSInDC /= self->configuration.usableRegions;
				if (self->primary) {
					// put extras in primary DC if it's uneven
					targetTSSInDC += (self->configuration.desiredTSSCount % self->configuration.usableRegions);
				}
			}
			int newTssToRecruit = targetTSSInDC - self->tss_info_by_pair.size() - inProgressTSSCount;
			// FIXME: Should log this if the recruit count stays the same but the other numbers update?
			if (newTssToRecruit != tssToRecruit) {
				TraceEvent("TSS_RecruitUpdated", self->distributorId)
				    .detail("Desired", targetTSSInDC)
				    .detail("Existing", self->tss_info_by_pair.size())
				    .detail("InProgress", inProgressTSSCount)
				    .detail("NotStarted", newTssToRecruit);
				tssToRecruit = newTssToRecruit;

				// if we need to get rid of some TSS processes, signal to either cancel recruitment or kill existing TSS
				// processes
				if (!pendingTSSCheck && (tssToRecruit < 0 || self->zeroHealthyTeams->get()) &&
				    (self->isTssRecruiting || (self->zeroHealthyTeams->get() && self->tss_info_by_pair.size() > 0))) {
					checkTss = self->initialFailureReactionDelay;
				}
			}
			numSSPerAddr.clear();
			hasHealthyTeam = (self->healthyTeamCount != 0);
			RecruitStorageRequest rsr;
			std::set<AddressExclusion> exclusions;
			for (auto s = self->server_and_tss_info.begin(); s != self->server_and_tss_info.end(); ++s) {
				auto serverStatus = self->server_status.get(s->second->lastKnownInterface.id());
				if (serverStatus.excludeOnRecruit()) {
					TraceEvent(SevDebug, "DDRecruitExcl1")
					    .detail("Primary", self->primary)
					    .detail("Excluding", s->second->lastKnownInterface.address());
					auto addr = s->second->lastKnownInterface.stableAddress();
					AddressExclusion addrExcl(addr.ip, addr.port);
					exclusions.insert(addrExcl);
					numSSPerAddr[addrExcl]++; // increase from 0
				}
			}
			for (auto addr : self->recruitingLocalities) {
				exclusions.insert(AddressExclusion(addr.ip, addr.port));
			}

			auto excl = self->excludedServers.getKeys();
			for (const auto& s : excl) {
				if (self->excludedServers.get(s) != DDTeamCollection::Status::NONE) {
					TraceEvent(SevDebug, "DDRecruitExcl2")
					    .detail("Primary", self->primary)
					    .detail("Excluding", s.toString());
					exclusions.insert(s);
				}
			}

			// Exclude workers that have invalid locality
			for (auto& addr : self->invalidLocalityAddr) {
				TraceEvent(SevDebug, "DDRecruitExclInvalidAddr").detail("Excluding", addr.toString());
				exclusions.insert(addr);
			}

			rsr.criticalRecruitment = !hasHealthyTeam;
			for (auto it : exclusions) {
				rsr.excludeAddresses.push_back(it);
			}

			rsr.includeDCs = self->includedDCs;

			TraceEvent(rsr.criticalRecruitment ? SevWarn : SevInfo, "DDRecruiting")
			    .detail("Primary", self->primary)
			    .detail("State", "Sending request to CC")
			    .detail("Exclusions", rsr.excludeAddresses.size())
			    .detail("Critical", rsr.criticalRecruitment)
			    .detail("IncludedDCsSize", rsr.includeDCs.size());

			if (rsr.criticalRecruitment) {
				TraceEvent(SevWarn, "DDRecruitingEmergency", self->distributorId).detail("Primary", self->primary);
			}

			if (!fCandidateWorker.isValid() || fCandidateWorker.isReady() ||
			    rsr.excludeAddresses != lastRequest.excludeAddresses ||
			    rsr.criticalRecruitment != lastRequest.criticalRecruitment) {
				lastRequest = rsr;
				fCandidateWorker =
				    brokenPromiseToNever(recruitStorage->get().getReply(rsr, TaskPriority::DataDistribution));
			}

			choose {
				when(RecruitStorageReply candidateWorker = wait(fCandidateWorker)) {
					AddressExclusion candidateSSAddr(candidateWorker.worker.stableAddress().ip,
					                                 candidateWorker.worker.stableAddress().port);
					int numExistingSS = numSSPerAddr[candidateSSAddr];
					if (numExistingSS >= 2) {
						TraceEvent(SevWarnAlways, "StorageRecruiterTooManySSOnSameAddr", self->distributorId)
						    .detail("Primary", self->primary)
						    .detail("Addr", candidateSSAddr.toString())
						    .detail("NumExistingSS", numExistingSS);
					}

					if (hasHealthyTeam && !tssState->active && tssToRecruit > 0) {
						TraceEvent("TSS_Recruit", self->distributorId)
						    .detail("Stage", "HoldTSS")
						    .detail("Addr", candidateSSAddr.toString())
						    .detail("Locality", candidateWorker.worker.locality.toString());

						TEST(true); // Starting TSS recruitment
						self->isTssRecruiting = true;
						tssState = makeReference<TSSPairState>(candidateWorker.worker.locality);

						addTSSInProgress.send(tssState->waitComplete());
						self->addActor.send(initializeStorage(self, candidateWorker, ddEnabledState, true, tssState));
						checkTss = self->initialFailureReactionDelay;
					} else {
						if (tssState->active && tssState->inDataZone(candidateWorker.worker.locality)) {
							TEST(true); // TSS recruits pair in same dc/datahall
							self->isTssRecruiting = false;
							TraceEvent("TSS_Recruit", self->distributorId)
							    .detail("Stage", "PairSS")
							    .detail("Addr", candidateSSAddr.toString())
							    .detail("Locality", candidateWorker.worker.locality.toString());
							self->addActor.send(
							    initializeStorage(self, candidateWorker, ddEnabledState, false, tssState));
							// successfully started recruitment of pair, reset tss recruitment state
							tssState = makeReference<TSSPairState>();
						} else {
							TEST(tssState->active); // TSS recruitment skipped potential pair because it's in a
							                        // different dc/datahall
							self->addActor.send(initializeStorage(
							    self, candidateWorker, ddEnabledState, false, makeReference<TSSPairState>()));
						}
					}
				}
				when(wait(recruitStorage->onChange())) { fCandidateWorker = Future<RecruitStorageReply>(); }
				when(wait(self->zeroHealthyTeams->onChange())) {
					if (!pendingTSSCheck && self->zeroHealthyTeams->get() &&
					    (self->isTssRecruiting || self->tss_info_by_pair.size() > 0)) {
						checkTss = self->initialFailureReactionDelay;
					}
				}
				when(wait(checkTss)) {
					bool cancelTss = self->isTssRecruiting && (tssToRecruit < 0 || self->zeroHealthyTeams->get());
					// Can't kill more tss' than we have. Kill 1 if zero healthy teams, otherwise kill enough to get
					// back to the desired amount
					int tssToKill = std::min((int)self->tss_info_by_pair.size(),
					                         std::max(-tssToRecruit, self->zeroHealthyTeams->get() ? 1 : 0));
					if (cancelTss) {
						TEST(tssToRecruit < 0); // tss recruitment cancelled due to too many TSS
						TEST(self->zeroHealthyTeams->get()); // tss recruitment cancelled due zero healthy teams

						TraceEvent(SevWarn, "TSS_RecruitCancelled", self->distributorId)
						    .detail("Reason", tssToRecruit <= 0 ? "TooMany" : "ZeroHealthyTeams");
						tssState->cancel();
						tssState = makeReference<TSSPairState>();
						self->isTssRecruiting = false;

						pendingTSSCheck = true;
						checkTss = delay(SERVER_KNOBS->TSS_DD_CHECK_INTERVAL);
					} else if (tssToKill > 0) {
						auto itr = self->tss_info_by_pair.begin();
						for (int i = 0; i < tssToKill; i++, itr++) {
							UID tssId = itr->second->id;
							StorageServerInterface tssi = itr->second->lastKnownInterface;

							if (self->shouldHandleServer(tssi) && self->server_and_tss_info.count(tssId)) {
								Promise<Void> killPromise = itr->second->killTss;
								if (killPromise.canBeSet()) {
									TEST(tssToRecruit < 0); // Killing TSS due to too many TSS
									TEST(self->zeroHealthyTeams->get()); // Killing TSS due zero healthy teams
									TraceEvent(SevWarn, "TSS_DDKill", self->distributorId)
									    .detail("TSSID", tssId)
									    .detail("Reason",
									            self->zeroHealthyTeams->get() ? "ZeroHealthyTeams" : "TooMany");
									killPromise.send(Void());
								}
							}
						}
						// If we're killing a TSS because of zero healthy teams, wait a bit to give the replacing SS a
						// change to join teams and stuff before killing another TSS
						pendingTSSCheck = true;
						checkTss = delay(SERVER_KNOBS->TSS_DD_CHECK_INTERVAL);
					} else if (self->isTssRecruiting) {
						// check again later in case we need to cancel recruitment
						pendingTSSCheck = true;
						checkTss = delay(SERVER_KNOBS->TSS_DD_CHECK_INTERVAL);
						// FIXME: better way to do this than timer?
					} else {
						pendingTSSCheck = false;
						checkTss = Never();
					}
				}
				when(wait(self->restartRecruiting.onTrigger())) {}
			}
			wait(delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY, TaskPriority::DataDistribution));
		} catch (Error& e) {
			if (e.code() != error_code_timed_out) {
				throw;
			}
			TEST(true); // Storage recruitment timed out
		}
	}
}

ACTOR Future<Void> updateReplicasKey(DDTeamCollection* self, Optional<Key> dcId) {
	std::vector<Future<Void>> serverUpdates;

	for (auto& it : self->server_info) {
		serverUpdates.push_back(it.second->updated.getFuture());
	}

	wait(self->initialFailureReactionDelay && waitForAll(serverUpdates));
	wait(waitUntilHealthy(self));
	TraceEvent("DDUpdatingReplicas", self->distributorId)
	    .detail("Primary", self->primary)
	    .detail("DcId", dcId)
	    .detail("Replicas", self->configuration.storageTeamSize);
	state Transaction tr(self->cx);
	loop {
		try {
			Optional<Value> val = wait(tr.get(datacenterReplicasKeyFor(dcId)));
			state int oldReplicas = val.present() ? decodeDatacenterReplicasValue(val.get()) : 0;
			if (oldReplicas == self->configuration.storageTeamSize) {
				TraceEvent("DDUpdatedAlready", self->distributorId)
				    .detail("Primary", self->primary)
				    .detail("DcId", dcId)
				    .detail("Replicas", self->configuration.storageTeamSize);
				return Void();
			}
			if (oldReplicas < self->configuration.storageTeamSize) {
				tr.set(rebootWhenDurableKey, StringRef());
			}
			tr.set(datacenterReplicasKeyFor(dcId), datacenterReplicasValue(self->configuration.storageTeamSize));
			wait(tr.commit());
			TraceEvent("DDUpdatedReplicas", self->distributorId)
			    .detail("Primary", self->primary)
			    .detail("DcId", dcId)
			    .detail("Replicas", self->configuration.storageTeamSize)
			    .detail("OldReplicas", oldReplicas);
			return Void();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<Void> serverGetTeamRequests(TeamCollectionInterface tci, DDTeamCollection* self) {
	loop {
		GetTeamRequest req = waitNext(tci.getTeam.getFuture());
		self->addActor.send(self->getTeam(self, req));
	}
}

ACTOR Future<Void> remoteRecovered(Reference<AsyncVar<ServerDBInfo> const> db) {
	TraceEvent("DDTrackerStarting").log();
	while (db->get().recoveryState < RecoveryState::ALL_LOGS_RECRUITED) {
		TraceEvent("DDTrackerStarting").detail("RecoveryState", (int)db->get().recoveryState);
		wait(db->onChange());
	}
	return Void();
}

ACTOR Future<Void> monitorHealthyTeams(DDTeamCollection* self) {
	TraceEvent("DDMonitorHealthyTeamsStart").detail("ZeroHealthyTeams", self->zeroHealthyTeams->get());
	loop choose {
		when(wait(self->zeroHealthyTeams->get()
		              ? delay(SERVER_KNOBS->DD_ZERO_HEALTHY_TEAM_DELAY, TaskPriority::DataDistribution)
		              : Never())) {
			self->doBuildTeams = true;
			wait(DDTeamCollection::checkBuildTeams(self));
		}
		when(wait(self->zeroHealthyTeams->onChange())) {}
	}
}

// Keep track of servers and teams -- serves requests for getRandomTeam
ACTOR Future<Void> dataDistributionTeamCollection(
    Reference<DDTeamCollection> teamCollection,
    Reference<InitialDataDistribution> initData,
    TeamCollectionInterface tci,
    Reference<IAsyncListener<RequestStream<RecruitStorageRequest>>> recruitStorage,
    DDEnabledState const* ddEnabledState) {
	state DDTeamCollection* self = teamCollection.getPtr();
	state Future<Void> loggingTrigger = Void();
	state PromiseStream<Void> serverRemoved;
	state Future<Void> error = actorCollection(self->addActor.getFuture());

	try {
		wait(DDTeamCollection::init(self, initData, ddEnabledState));
		initData = Reference<InitialDataDistribution>();
		self->addActor.send(serverGetTeamRequests(tci, self));

		TraceEvent("DDTeamCollectionBegin", self->distributorId).detail("Primary", self->primary);
		wait(self->readyToStart || error);
		TraceEvent("DDTeamCollectionReadyToStart", self->distributorId).detail("Primary", self->primary);

		// removeBadTeams() does not always run. We may need to restart the actor when needed.
		// So we need the badTeamRemover variable to check if the actor is ready.
		if (self->badTeamRemover.isReady()) {
			self->badTeamRemover = removeBadTeams(self);
			self->addActor.send(self->badTeamRemover);
		}

		self->addActor.send(machineTeamRemover(self));
		self->addActor.send(serverTeamRemover(self));

		if (self->wrongStoreTypeRemover.isReady()) {
			self->wrongStoreTypeRemover = removeWrongStoreType(self);
			self->addActor.send(self->wrongStoreTypeRemover);
		}

		self->traceTeamCollectionInfo();

		if (self->includedDCs.size()) {
			// start this actor before any potential recruitments can happen
			self->addActor.send(updateReplicasKey(self, self->includedDCs[0]));
		}

		// The following actors (e.g. storageRecruiter) do not need to be assigned to a variable because
		// they are always running.
		self->addActor.send(storageRecruiter(self, recruitStorage, ddEnabledState));
		self->addActor.send(monitorStorageServerRecruitment(self));
		self->addActor.send(waitServerListChange(self, serverRemoved.getFuture(), ddEnabledState));
		self->addActor.send(trackExcludedServers(self));
		self->addActor.send(monitorHealthyTeams(self));
		self->addActor.send(waitHealthyZoneChange(self));
		self->addActor.send(monitorPerpetualStorageWiggle(self));
		// SOMEDAY: Monitor FF/serverList for (new) servers that aren't in allServers and add or remove them

		loop choose {
			when(UID removedServer = waitNext(self->removedServers.getFuture())) {
				TEST(true); // Storage server removed from database
				self->removeServer(removedServer);
				serverRemoved.send(Void());

				self->restartRecruiting.trigger();
			}
			when(UID removedTSS = waitNext(self->removedTSS.getFuture())) {
				TEST(true); // TSS removed from database
				self->removeTSS(removedTSS);
				serverRemoved.send(Void());

				self->restartRecruiting.trigger();
			}
			when(wait(self->zeroHealthyTeams->onChange())) {
				if (self->zeroHealthyTeams->get()) {
					self->restartRecruiting.trigger();
					self->noHealthyTeams();
				}
			}
			when(wait(loggingTrigger)) {
				int highestPriority = 0;
				for (auto it : self->priority_teams) {
					if (it.second > 0) {
						highestPriority = std::max(highestPriority, it.first);
					}
				}

				TraceEvent("TotalDataInFlight", self->distributorId)
				    .detail("Primary", self->primary)
				    .detail("TotalBytes", self->getDebugTotalDataInFlight())
				    .detail("UnhealthyServers", self->unhealthyServers)
				    .detail("ServerCount", self->server_info.size())
				    .detail("StorageTeamSize", self->configuration.storageTeamSize)
				    .detail("HighestPriority", highestPriority)
				    .trackLatest(self->primary ? "TotalDataInFlight" : "TotalDataInFlightRemote");

				loggingTrigger = delay(SERVER_KNOBS->DATA_DISTRIBUTION_LOGGING_INTERVAL, TaskPriority::FlushTrace);
			}
			when(wait(self->serverTrackerErrorOut.getFuture())) {} // Propagate errors from storageServerTracker
			when(wait(error)) {}
		}
	} catch (Error& e) {
		if (e.code() != error_code_movekeys_conflict)
			TraceEvent(SevError, "DataDistributionTeamCollectionError", self->distributorId).error(e);
		throw e;
	}
}

ACTOR Future<Void> waitForDataDistributionEnabled(Database cx, const DDEnabledState* ddEnabledState) {
	state Transaction tr(cx);
	loop {
		wait(delay(SERVER_KNOBS->DD_ENABLED_CHECK_DELAY, TaskPriority::DataDistribution));

		try {
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (!mode.present() && ddEnabledState->isDDEnabled()) {
				TraceEvent("WaitForDDEnabledSucceeded").log();
				return Void();
			}
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				int m;
				rd >> m;
				TraceEvent(SevDebug, "WaitForDDEnabled")
				    .detail("Mode", m)
				    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
				if (m && ddEnabledState->isDDEnabled()) {
					TraceEvent("WaitForDDEnabledSucceeded").log();
					return Void();
				}
			}

			tr.reset();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<bool> isDataDistributionEnabled(Database cx, const DDEnabledState* ddEnabledState) {
	state Transaction tr(cx);
	loop {
		try {
			Optional<Value> mode = wait(tr.get(dataDistributionModeKey));
			if (!mode.present() && ddEnabledState->isDDEnabled())
				return true;
			if (mode.present()) {
				BinaryReader rd(mode.get(), Unversioned());
				int m;
				rd >> m;
				if (m && ddEnabledState->isDDEnabled()) {
					TraceEvent(SevDebug, "IsDDEnabledSucceeded")
					    .detail("Mode", m)
					    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
					return true;
				}
			}
			// SOMEDAY: Write a wrapper in MoveKeys.actor.h
			Optional<Value> readVal = wait(tr.get(moveKeysLockOwnerKey));
			UID currentOwner =
			    readVal.present() ? BinaryReader::fromStringRef<UID>(readVal.get(), Unversioned()) : UID();
			if (ddEnabledState->isDDEnabled() && (currentOwner != dataDistributionModeLock)) {
				TraceEvent(SevDebug, "IsDDEnabledSucceeded")
				    .detail("CurrentOwner", currentOwner)
				    .detail("DDModeLock", dataDistributionModeLock)
				    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
				return true;
			}
			TraceEvent(SevDebug, "IsDDEnabledFailed")
			    .detail("CurrentOwner", currentOwner)
			    .detail("DDModeLock", dataDistributionModeLock)
			    .detail("IsDDEnabled", ddEnabledState->isDDEnabled());
			return false;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

// Ensures that the serverKeys key space is properly coalesced
// This method is only used for testing and is not implemented in a manner that is safe for large databases
ACTOR Future<Void> debugCheckCoalescing(Database cx) {
	state Transaction tr(cx);
	loop {
		try {
			state RangeResult serverList = wait(tr.getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY);

			state int i;
			for (i = 0; i < serverList.size(); i++) {
				state UID id = decodeServerListValue(serverList[i].value).id();
				RangeResult ranges = wait(krmGetRanges(&tr, serverKeysPrefixFor(id), allKeys));
				ASSERT(ranges.end()[-1].key == allKeys.end);

				for (int j = 0; j < ranges.size() - 2; j++)
					if (ranges[j].value == ranges[j + 1].value)
						TraceEvent(SevError, "UncoalescedValues", id)
						    .detail("Key1", ranges[j].key)
						    .detail("Key2", ranges[j + 1].key)
						    .detail("Value", ranges[j].value);
			}

			TraceEvent("DoneCheckingCoalescing").log();
			return Void();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

static std::set<int> const& normalDDQueueErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert(error_code_movekeys_conflict);
		s.insert(error_code_broken_promise);
	}
	return s;
}

ACTOR Future<Void> pollMoveKeysLock(Database cx, MoveKeysLock lock, const DDEnabledState* ddEnabledState) {
	loop {
		wait(delay(SERVER_KNOBS->MOVEKEYS_LOCK_POLLING_DELAY));
		state Transaction tr(cx);
		loop {
			try {
				wait(checkMoveKeysLockReadOnly(&tr, lock, ddEnabledState));
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}
}

struct DataDistributorData : NonCopyable, ReferenceCounted<DataDistributorData> {
	Reference<AsyncVar<ServerDBInfo> const> dbInfo;
	UID ddId;
	PromiseStream<Future<Void>> addActor;
	DDTeamCollection* teamCollection;

	DataDistributorData(Reference<AsyncVar<ServerDBInfo> const> const& db, UID id)
	  : dbInfo(db), ddId(id), teamCollection(nullptr) {}
};

ACTOR Future<Void> monitorBatchLimitedTime(Reference<AsyncVar<ServerDBInfo> const> db, double* lastLimited) {
	loop {
		wait(delay(SERVER_KNOBS->METRIC_UPDATE_RATE));

		state Reference<GrvProxyInfo> grvProxies(new GrvProxyInfo(db->get().client.grvProxies, false));

		choose {
			when(wait(db->onChange())) {}
			when(GetHealthMetricsReply reply =
			         wait(grvProxies->size() ? basicLoadBalance(grvProxies,
			                                                    &GrvProxyInterface::getHealthMetrics,
			                                                    GetHealthMetricsRequest(false))
			                                 : Never())) {
				if (reply.healthMetrics.batchLimited) {
					*lastLimited = now();
				}
			}
		}
	}
}

// Runs the data distribution algorithm for FDB, including the DD Queue, DD tracker, and DD team collection
ACTOR Future<Void> dataDistribution(Reference<DataDistributorData> self,
                                    PromiseStream<GetMetricsListRequest> getShardMetricsList,
                                    const DDEnabledState* ddEnabledState) {
	state double lastLimited = 0;
	self->addActor.send(monitorBatchLimitedTime(self->dbInfo, &lastLimited));

	state Database cx = openDBOnServer(self->dbInfo, TaskPriority::DataDistributionLaunch, LockAware::True);
	cx->locationCacheSize = SERVER_KNOBS->DD_LOCATION_CACHE_SIZE;

	// cx->setOption( FDBDatabaseOptions::LOCATION_CACHE_SIZE, StringRef((uint8_t*)
	// &SERVER_KNOBS->DD_LOCATION_CACHE_SIZE, 8) ); ASSERT( cx->locationCacheSize ==
	// SERVER_KNOBS->DD_LOCATION_CACHE_SIZE
	// );

	// wait(debugCheckCoalescing(cx));
	state std::vector<Optional<Key>> primaryDcId;
	state std::vector<Optional<Key>> remoteDcIds;
	state DatabaseConfiguration configuration;
	state Reference<InitialDataDistribution> initData;
	state MoveKeysLock lock;
	state Reference<DDTeamCollection> primaryTeamCollection;
	state Reference<DDTeamCollection> remoteTeamCollection;
	state bool trackerCancelled;
	loop {
		trackerCancelled = false;

		// Stored outside of data distribution tracker to avoid slow tasks
		// when tracker is cancelled
		state KeyRangeMap<ShardTrackedData> shards;
		state Promise<UID> removeFailedServer;
		try {
			loop {
				TraceEvent("DDInitTakingMoveKeysLock", self->ddId).log();
				MoveKeysLock lock_ = wait(takeMoveKeysLock(cx, self->ddId));
				lock = lock_;
				TraceEvent("DDInitTookMoveKeysLock", self->ddId).log();

				DatabaseConfiguration configuration_ = wait(getDatabaseConfiguration(cx));
				configuration = configuration_;
				primaryDcId.clear();
				remoteDcIds.clear();
				const std::vector<RegionInfo>& regions = configuration.regions;
				if (configuration.regions.size() > 0) {
					primaryDcId.push_back(regions[0].dcId);
				}
				if (configuration.regions.size() > 1) {
					remoteDcIds.push_back(regions[1].dcId);
				}

				TraceEvent("DDInitGotConfiguration", self->ddId).detail("Conf", configuration.toString());

				state Transaction tr(cx);
				loop {
					try {
						tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
						tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

						RangeResult replicaKeys = wait(tr.getRange(datacenterReplicasKeys, CLIENT_KNOBS->TOO_MANY));

						for (auto& kv : replicaKeys) {
							auto dcId = decodeDatacenterReplicasKey(kv.key);
							auto replicas = decodeDatacenterReplicasValue(kv.value);
							if ((primaryDcId.size() && primaryDcId[0] == dcId) ||
							    (remoteDcIds.size() && remoteDcIds[0] == dcId && configuration.usableRegions > 1)) {
								if (replicas > configuration.storageTeamSize) {
									tr.set(kv.key, datacenterReplicasValue(configuration.storageTeamSize));
								}
							} else {
								tr.clear(kv.key);
							}
						}

						wait(tr.commit());
						break;
					} catch (Error& e) {
						wait(tr.onError(e));
					}
				}

				TraceEvent("DDInitUpdatedReplicaKeys", self->ddId).log();
				Reference<InitialDataDistribution> initData_ = wait(getInitialDataDistribution(
				    cx,
				    self->ddId,
				    lock,
				    configuration.usableRegions > 1 ? remoteDcIds : std::vector<Optional<Key>>(),
				    ddEnabledState));
				initData = initData_;
				if (initData->shards.size() > 1) {
					TraceEvent("DDInitGotInitialDD", self->ddId)
					    .detail("B", initData->shards.end()[-2].key)
					    .detail("E", initData->shards.end()[-1].key)
					    .detail("Src", describe(initData->shards.end()[-2].primarySrc))
					    .detail("Dest", describe(initData->shards.end()[-2].primaryDest))
					    .trackLatest("InitialDD");
				} else {
					TraceEvent("DDInitGotInitialDD", self->ddId)
					    .detail("B", "")
					    .detail("E", "")
					    .detail("Src", "[no items]")
					    .detail("Dest", "[no items]")
					    .trackLatest("InitialDD");
				}

				if (initData->mode && ddEnabledState->isDDEnabled()) {
					// mode may be set true by system operator using fdbcli and isDDEnabled() set to true
					break;
				}
				TraceEvent("DataDistributionDisabled", self->ddId).log();

				TraceEvent("MovingData", self->ddId)
				    .detail("InFlight", 0)
				    .detail("InQueue", 0)
				    .detail("AverageShardSize", -1)
				    .detail("UnhealthyRelocations", 0)
				    .detail("HighestPriority", 0)
				    .detail("BytesWritten", 0)
				    .detail("PriorityRecoverMove", 0)
				    .detail("PriorityRebalanceUnderutilizedTeam", 0)
				    .detail("PriorityRebalannceOverutilizedTeam", 0)
				    .detail("PriorityTeamHealthy", 0)
				    .detail("PriorityTeamContainsUndesiredServer", 0)
				    .detail("PriorityTeamRedundant", 0)
				    .detail("PriorityMergeShard", 0)
				    .detail("PriorityTeamUnhealthy", 0)
				    .detail("PriorityTeam2Left", 0)
				    .detail("PriorityTeam1Left", 0)
				    .detail("PriorityTeam0Left", 0)
				    .detail("PrioritySplitShard", 0)
				    .trackLatest("MovingData");

				TraceEvent("TotalDataInFlight", self->ddId)
				    .detail("Primary", true)
				    .detail("TotalBytes", 0)
				    .detail("UnhealthyServers", 0)
				    .detail("HighestPriority", 0)
				    .trackLatest("TotalDataInFlight");
				TraceEvent("TotalDataInFlight", self->ddId)
				    .detail("Primary", false)
				    .detail("TotalBytes", 0)
				    .detail("UnhealthyServers", 0)
				    .detail("HighestPriority", configuration.usableRegions > 1 ? 0 : -1)
				    .trackLatest("TotalDataInFlightRemote");

				wait(waitForDataDistributionEnabled(cx, ddEnabledState));
				TraceEvent("DataDistributionEnabled").log();
			}

			// When/If this assertion fails, Evan owes Ben a pat on the back for his foresight
			ASSERT(configuration.storageTeamSize > 0);

			state PromiseStream<RelocateShard> output;
			state PromiseStream<RelocateShard> input;
			state PromiseStream<Promise<int64_t>> getAverageShardBytes;
			state PromiseStream<Promise<int>> getUnhealthyRelocationCount;
			state PromiseStream<GetMetricsRequest> getShardMetrics;
			state Reference<AsyncVar<bool>> processingUnhealthy(new AsyncVar<bool>(false));
			state Promise<Void> readyToStart;
			state Reference<ShardsAffectedByTeamFailure> shardsAffectedByTeamFailure(new ShardsAffectedByTeamFailure);

			state int shard = 0;
			for (; shard < initData->shards.size() - 1; shard++) {
				KeyRangeRef keys = KeyRangeRef(initData->shards[shard].key, initData->shards[shard + 1].key);
				shardsAffectedByTeamFailure->defineShard(keys);
				std::vector<ShardsAffectedByTeamFailure::Team> teams;
				teams.push_back(ShardsAffectedByTeamFailure::Team(initData->shards[shard].primarySrc, true));
				if (configuration.usableRegions > 1) {
					teams.push_back(ShardsAffectedByTeamFailure::Team(initData->shards[shard].remoteSrc, false));
				}
				if (g_network->isSimulated()) {
					TraceEvent("DDInitShard")
					    .detail("Keys", keys)
					    .detail("PrimarySrc", describe(initData->shards[shard].primarySrc))
					    .detail("RemoteSrc", describe(initData->shards[shard].remoteSrc))
					    .detail("PrimaryDest", describe(initData->shards[shard].primaryDest))
					    .detail("RemoteDest", describe(initData->shards[shard].remoteDest));
				}

				shardsAffectedByTeamFailure->moveShard(keys, teams);
				if (initData->shards[shard].hasDest) {
					// This shard is already in flight.  Ideally we should use dest in ShardsAffectedByTeamFailure and
					// generate a dataDistributionRelocator directly in DataDistributionQueue to track it, but it's
					// easier to just (with low priority) schedule it for movement.
					bool unhealthy = initData->shards[shard].primarySrc.size() != configuration.storageTeamSize;
					if (!unhealthy && configuration.usableRegions > 1) {
						unhealthy = initData->shards[shard].remoteSrc.size() != configuration.storageTeamSize;
					}
					output.send(RelocateShard(
					    keys, unhealthy ? SERVER_KNOBS->PRIORITY_TEAM_UNHEALTHY : SERVER_KNOBS->PRIORITY_RECOVER_MOVE));
				}
				wait(yield(TaskPriority::DataDistribution));
			}

			std::vector<TeamCollectionInterface> tcis;

			Reference<AsyncVar<bool>> anyZeroHealthyTeams;
			std::vector<Reference<AsyncVar<bool>>> zeroHealthyTeams;
			tcis.push_back(TeamCollectionInterface());
			zeroHealthyTeams.push_back(makeReference<AsyncVar<bool>>(true));
			int storageTeamSize = configuration.storageTeamSize;

			std::vector<Future<Void>> actors;
			if (configuration.usableRegions > 1) {
				tcis.push_back(TeamCollectionInterface());
				storageTeamSize = 2 * configuration.storageTeamSize;

				zeroHealthyTeams.push_back(makeReference<AsyncVar<bool>>(true));
				anyZeroHealthyTeams = makeReference<AsyncVar<bool>>(true);
				actors.push_back(anyTrue(zeroHealthyTeams, anyZeroHealthyTeams));
			} else {
				anyZeroHealthyTeams = zeroHealthyTeams[0];
			}

			actors.push_back(pollMoveKeysLock(cx, lock, ddEnabledState));
			actors.push_back(reportErrorsExcept(dataDistributionTracker(initData,
			                                                            cx,
			                                                            output,
			                                                            shardsAffectedByTeamFailure,
			                                                            getShardMetrics,
			                                                            getShardMetricsList,
			                                                            getAverageShardBytes.getFuture(),
			                                                            readyToStart,
			                                                            anyZeroHealthyTeams,
			                                                            self->ddId,
			                                                            &shards,
			                                                            &trackerCancelled),
			                                    "DDTracker",
			                                    self->ddId,
			                                    &normalDDQueueErrors()));
			actors.push_back(reportErrorsExcept(dataDistributionQueue(cx,
			                                                          output,
			                                                          input.getFuture(),
			                                                          getShardMetrics,
			                                                          processingUnhealthy,
			                                                          tcis,
			                                                          shardsAffectedByTeamFailure,
			                                                          lock,
			                                                          getAverageShardBytes,
			                                                          getUnhealthyRelocationCount,
			                                                          self->ddId,
			                                                          storageTeamSize,
			                                                          configuration.storageTeamSize,
			                                                          &lastLimited,
			                                                          ddEnabledState),
			                                    "DDQueue",
			                                    self->ddId,
			                                    &normalDDQueueErrors()));

			std::vector<DDTeamCollection*> teamCollectionsPtrs;
			primaryTeamCollection = makeReference<DDTeamCollection>(
			    cx,
			    self->ddId,
			    lock,
			    output,
			    shardsAffectedByTeamFailure,
			    configuration,
			    primaryDcId,
			    configuration.usableRegions > 1 ? remoteDcIds : std::vector<Optional<Key>>(),
			    readyToStart.getFuture(),
			    zeroHealthyTeams[0],
			    IsPrimary::True,
			    processingUnhealthy,
			    getShardMetrics,
			    removeFailedServer,
			    getUnhealthyRelocationCount);
			teamCollectionsPtrs.push_back(primaryTeamCollection.getPtr());
			auto recruitStorage = IAsyncListener<RequestStream<RecruitStorageRequest>>::create(
			    self->dbInfo, [](auto const& info) { return info.clusterInterface.recruitStorage; });
			if (configuration.usableRegions > 1) {
				remoteTeamCollection =
				    makeReference<DDTeamCollection>(cx,
				                                    self->ddId,
				                                    lock,
				                                    output,
				                                    shardsAffectedByTeamFailure,
				                                    configuration,
				                                    remoteDcIds,
				                                    Optional<std::vector<Optional<Key>>>(),
				                                    readyToStart.getFuture() && remoteRecovered(self->dbInfo),
				                                    zeroHealthyTeams[1],
				                                    IsPrimary::False,
				                                    processingUnhealthy,
				                                    getShardMetrics,
				                                    removeFailedServer,
				                                    getUnhealthyRelocationCount);
				teamCollectionsPtrs.push_back(remoteTeamCollection.getPtr());
				remoteTeamCollection->teamCollections = teamCollectionsPtrs;
				actors.push_back(
				    reportErrorsExcept(dataDistributionTeamCollection(
				                           remoteTeamCollection, initData, tcis[1], recruitStorage, ddEnabledState),
				                       "DDTeamCollectionSecondary",
				                       self->ddId,
				                       &normalDDQueueErrors()));
				actors.push_back(printSnapshotTeamsInfo(remoteTeamCollection));
			}
			primaryTeamCollection->teamCollections = teamCollectionsPtrs;
			self->teamCollection = primaryTeamCollection.getPtr();
			actors.push_back(
			    reportErrorsExcept(dataDistributionTeamCollection(
			                           primaryTeamCollection, initData, tcis[0], recruitStorage, ddEnabledState),
			                       "DDTeamCollectionPrimary",
			                       self->ddId,
			                       &normalDDQueueErrors()));

			actors.push_back(printSnapshotTeamsInfo(primaryTeamCollection));
			actors.push_back(yieldPromiseStream(output.getFuture(), input));

			wait(waitForAll(actors));
			return Void();
		} catch (Error& e) {
			trackerCancelled = true;
			state Error err = e;
			TraceEvent("DataDistributorDestroyTeamCollections").error(e);
			self->teamCollection = nullptr;
			primaryTeamCollection = Reference<DDTeamCollection>();
			remoteTeamCollection = Reference<DDTeamCollection>();
			wait(shards.clearAsync());
			TraceEvent("DataDistributorTeamCollectionsDestroyed").error(err);
			if (removeFailedServer.getFuture().isReady() && !removeFailedServer.getFuture().isError()) {
				TraceEvent("RemoveFailedServer", removeFailedServer.getFuture().get()).error(err);
				wait(removeKeysFromFailedServer(cx, removeFailedServer.getFuture().get(), lock, ddEnabledState));
				Optional<UID> tssPairID;
				wait(removeStorageServer(cx, removeFailedServer.getFuture().get(), tssPairID, lock, ddEnabledState));
			} else {
				if (err.code() != error_code_movekeys_conflict) {
					throw err;
				}

				bool ddEnabled = wait(isDataDistributionEnabled(cx, ddEnabledState));
				TraceEvent("DataDistributionMoveKeysConflict").detail("DataDistributionEnabled", ddEnabled).error(err);
				if (ddEnabled) {
					throw err;
				}
			}
		}
	}
}

static std::set<int> const& normalDataDistributorErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert(error_code_worker_removed);
		s.insert(error_code_broken_promise);
		s.insert(error_code_actor_cancelled);
		s.insert(error_code_please_reboot);
		s.insert(error_code_movekeys_conflict);
	}
	return s;
}

ACTOR Future<Void> ddSnapCreateCore(DistributorSnapRequest snapReq, Reference<AsyncVar<ServerDBInfo> const> db) {
	state Database cx = openDBOnServer(db, TaskPriority::DefaultDelay, LockAware::True);
	state ReadYourWritesTransaction tr(cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			TraceEvent("SnapDataDistributor_WriteFlagAttempt")
			    .detail("SnapPayload", snapReq.snapPayload)
			    .detail("SnapUID", snapReq.snapUID);
			tr.set(writeRecoveryKey, writeRecoveryKeyTrue);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			TraceEvent("SnapDataDistributor_WriteFlagError").error(e);
			wait(tr.onError(e));
		}
	}
	TraceEvent("SnapDataDistributor_SnapReqEnter")
	    .detail("SnapPayload", snapReq.snapPayload)
	    .detail("SnapUID", snapReq.snapUID);
	try {
		// disable tlog pop on local tlog nodes
		state std::vector<TLogInterface> tlogs = db->get().logSystemConfig.allLocalLogs(false);
		std::vector<Future<Void>> disablePops;
		disablePops.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			disablePops.push_back(transformErrors(
			    throwErrorOr(tlog.disablePopRequest.tryGetReply(TLogDisablePopRequest(snapReq.snapUID))),
			    snap_disable_tlog_pop_failed()));
		}
		wait(waitForAll(disablePops));

		TraceEvent("SnapDataDistributor_AfterDisableTLogPop")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap local storage nodes
		std::vector<WorkerInterface> storageWorkers =
		    wait(transformErrors(getStorageWorkers(cx, db, true /* localOnly */), snap_storage_failed()));
		TraceEvent("SnapDataDistributor_GotStorageWorkers")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		std::vector<Future<Void>> storageSnapReqs;
		storageSnapReqs.reserve(storageWorkers.size());
		for (const auto& worker : storageWorkers) {
			storageSnapReqs.push_back(
			    transformErrors(throwErrorOr(worker.workerSnapReq.tryGetReply(WorkerSnapRequest(
			                        snapReq.snapPayload, snapReq.snapUID, LiteralStringRef("storage")))),
			                    snap_storage_failed()));
		}
		wait(waitForAll(storageSnapReqs));

		TraceEvent("SnapDataDistributor_AfterSnapStorage")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap local tlog nodes
		std::vector<Future<Void>> tLogSnapReqs;
		tLogSnapReqs.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			tLogSnapReqs.push_back(
			    transformErrors(throwErrorOr(tlog.snapRequest.tryGetReply(
			                        TLogSnapRequest(snapReq.snapPayload, snapReq.snapUID, LiteralStringRef("tlog")))),
			                    snap_tlog_failed()));
		}
		wait(waitForAll(tLogSnapReqs));

		TraceEvent("SnapDataDistributor_AfterTLogStorage")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// enable tlog pop on local tlog nodes
		std::vector<Future<Void>> enablePops;
		enablePops.reserve(tlogs.size());
		for (const auto& tlog : tlogs) {
			enablePops.push_back(
			    transformErrors(throwErrorOr(tlog.enablePopRequest.tryGetReply(TLogEnablePopRequest(snapReq.snapUID))),
			                    snap_enable_tlog_pop_failed()));
		}
		wait(waitForAll(enablePops));

		TraceEvent("SnapDataDistributor_AfterEnableTLogPops")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		// snap the coordinators
		std::vector<WorkerInterface> coordWorkers = wait(getCoordWorkers(cx, db));
		TraceEvent("SnapDataDistributor_GotCoordWorkers")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		std::vector<Future<Void>> coordSnapReqs;
		coordSnapReqs.reserve(coordWorkers.size());
		for (const auto& worker : coordWorkers) {
			coordSnapReqs.push_back(
			    transformErrors(throwErrorOr(worker.workerSnapReq.tryGetReply(WorkerSnapRequest(
			                        snapReq.snapPayload, snapReq.snapUID, LiteralStringRef("coord")))),
			                    snap_coord_failed()));
		}
		wait(waitForAll(coordSnapReqs));
		TraceEvent("SnapDataDistributor_AfterSnapCoords")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID);
		tr.reset();
		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				TraceEvent("SnapDataDistributor_ClearFlagAttempt")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				tr.clear(writeRecoveryKey);
				wait(tr.commit());
				break;
			} catch (Error& e) {
				TraceEvent("SnapDataDistributor_ClearFlagError").error(e);
				wait(tr.onError(e));
			}
		}
	} catch (Error& err) {
		state Error e = err;
		TraceEvent("SnapDataDistributor_SnapReqExit")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID)
		    .error(e, true /*includeCancelled */);
		if (e.code() == error_code_snap_storage_failed || e.code() == error_code_snap_tlog_failed ||
		    e.code() == error_code_operation_cancelled || e.code() == error_code_snap_disable_tlog_pop_failed) {
			// enable tlog pop on local tlog nodes
			std::vector<TLogInterface> tlogs = db->get().logSystemConfig.allLocalLogs(false);
			try {
				std::vector<Future<Void>> enablePops;
				enablePops.reserve(tlogs.size());
				for (const auto& tlog : tlogs) {
					enablePops.push_back(transformErrors(
					    throwErrorOr(tlog.enablePopRequest.tryGetReply(TLogEnablePopRequest(snapReq.snapUID))),
					    snap_enable_tlog_pop_failed()));
				}
				wait(waitForAll(enablePops));
			} catch (Error& error) {
				TraceEvent(SevDebug, "IgnoreEnableTLogPopFailure").log();
			}
		}
		throw e;
	}
	return Void();
}

ACTOR Future<Void> ddSnapCreate(DistributorSnapRequest snapReq,
                                Reference<AsyncVar<ServerDBInfo> const> db,
                                DDEnabledState* ddEnabledState) {
	state Future<Void> dbInfoChange = db->onChange();
	if (!ddEnabledState->setDDEnabled(false, snapReq.snapUID)) {
		// disable DD before doing snapCreate, if previous snap req has already disabled DD then this operation fails
		// here
		TraceEvent("SnapDDSetDDEnabledFailedInMemoryCheck").log();
		snapReq.reply.sendError(operation_failed());
		return Void();
	}
	double delayTime = g_network->isSimulated() ? 70.0 : SERVER_KNOBS->SNAP_CREATE_MAX_TIMEOUT;
	try {
		choose {
			when(wait(dbInfoChange)) {
				TraceEvent("SnapDDCreateDBInfoChanged")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.sendError(snap_with_recovery_unsupported());
			}
			when(wait(ddSnapCreateCore(snapReq, db))) {
				TraceEvent("SnapDDCreateSuccess")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.send(Void());
			}
			when(wait(delay(delayTime))) {
				TraceEvent("SnapDDCreateTimedOut")
				    .detail("SnapPayload", snapReq.snapPayload)
				    .detail("SnapUID", snapReq.snapUID);
				snapReq.reply.sendError(timed_out());
			}
		}
	} catch (Error& e) {
		TraceEvent("SnapDDCreateError")
		    .detail("SnapPayload", snapReq.snapPayload)
		    .detail("SnapUID", snapReq.snapUID)
		    .error(e, true /*includeCancelled */);
		if (e.code() != error_code_operation_cancelled) {
			snapReq.reply.sendError(e);
		} else {
			// enable DD should always succeed
			bool success = ddEnabledState->setDDEnabled(true, snapReq.snapUID);
			ASSERT(success);
			throw e;
		}
	}
	// enable DD should always succeed
	bool success = ddEnabledState->setDDEnabled(true, snapReq.snapUID);
	ASSERT(success);
	return Void();
}

// Find size of set intersection of excludeServerIDs and serverIDs on each team and see if the leftover team is valid
bool _exclusionSafetyCheck(std::vector<UID>& excludeServerIDs, DDTeamCollection* teamCollection) {
	std::sort(excludeServerIDs.begin(), excludeServerIDs.end());
	for (const auto& team : teamCollection->teams) {
		std::vector<UID> teamServerIDs = team->getServerIDs();
		std::sort(teamServerIDs.begin(), teamServerIDs.end());
		TraceEvent(SevDebug, "DDExclusionSafetyCheck", teamCollection->distributorId)
		    .detail("Excluding", describe(excludeServerIDs))
		    .detail("Existing", team->getDesc());
		// Find size of set intersection of both vectors and see if the leftover team is valid
		std::vector<UID> intersectSet(teamServerIDs.size());
		auto it = std::set_intersection(excludeServerIDs.begin(),
		                                excludeServerIDs.end(),
		                                teamServerIDs.begin(),
		                                teamServerIDs.end(),
		                                intersectSet.begin());
		intersectSet.resize(it - intersectSet.begin());
		if (teamServerIDs.size() - intersectSet.size() < SERVER_KNOBS->DD_EXCLUDE_MIN_REPLICAS) {
			return false;
		}
	}
	return true;
}

ACTOR Future<Void> ddExclusionSafetyCheck(DistributorExclusionSafetyCheckRequest req,
                                          Reference<DataDistributorData> self,
                                          Database cx) {
	TraceEvent("DDExclusionSafetyCheckBegin", self->ddId).log();
	std::vector<StorageServerInterface> ssis = wait(getStorageServers(cx));
	DistributorExclusionSafetyCheckReply reply(true);
	if (!self->teamCollection) {
		TraceEvent("DDExclusionSafetyCheckTeamCollectionInvalid", self->ddId).log();
		reply.safe = false;
		req.reply.send(reply);
		return Void();
	}
	// If there is only 1 team, unsafe to mark failed: team building can get stuck due to lack of servers left
	if (self->teamCollection->teams.size() <= 1) {
		TraceEvent("DDExclusionSafetyCheckNotEnoughTeams", self->ddId).log();
		reply.safe = false;
		req.reply.send(reply);
		return Void();
	}
	std::vector<UID> excludeServerIDs;
	// Go through storage server interfaces and translate Address -> server ID (UID)
	for (const AddressExclusion& excl : req.exclusions) {
		for (const auto& ssi : ssis) {
			if (excl.excludes(ssi.address()) ||
			    (ssi.secondaryAddress().present() && excl.excludes(ssi.secondaryAddress().get()))) {
				excludeServerIDs.push_back(ssi.id());
			}
		}
	}
	reply.safe = _exclusionSafetyCheck(excludeServerIDs, self->teamCollection);
	TraceEvent("DDExclusionSafetyCheckFinish", self->ddId).log();
	req.reply.send(reply);
	return Void();
}

ACTOR Future<Void> waitFailCacheServer(Database* db, StorageServerInterface ssi) {
	state Transaction tr(*db);
	state Key key = storageCacheServerKey(ssi.id());
	wait(waitFailureClient(ssi.waitFailure));
	loop {
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		try {
			tr.addReadConflictRange(storageCacheServerKeys);
			tr.clear(key);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
	return Void();
}

ACTOR Future<Void> cacheServerWatcher(Database* db) {
	state Transaction tr(*db);
	state ActorCollection actors(false);
	state std::set<UID> knownCaches;
	loop {
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		try {
			RangeResult range = wait(tr.getRange(storageCacheServerKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!range.more);
			std::set<UID> caches;
			for (auto& kv : range) {
				UID id;
				BinaryReader reader{ kv.key.removePrefix(storageCacheServersPrefix), Unversioned() };
				reader >> id;
				caches.insert(id);
				if (knownCaches.find(id) == knownCaches.end()) {
					StorageServerInterface ssi;
					BinaryReader reader{ kv.value, IncludeVersion() };
					reader >> ssi;
					actors.add(waitFailCacheServer(db, ssi));
				}
			}
			knownCaches = std::move(caches);
			tr.reset();
			wait(delay(5.0) || actors.getResult());
			ASSERT(!actors.getResult().isReady());
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

static int64_t getMedianShardSize(VectorRef<DDMetricsRef> metricVec) {
	std::nth_element(metricVec.begin(),
	                 metricVec.begin() + metricVec.size() / 2,
	                 metricVec.end(),
	                 [](const DDMetricsRef& d1, const DDMetricsRef& d2) { return d1.shardBytes < d2.shardBytes; });
	return metricVec[metricVec.size() / 2].shardBytes;
}

ACTOR Future<Void> ddGetMetrics(GetDataDistributorMetricsRequest req,
                                PromiseStream<GetMetricsListRequest> getShardMetricsList) {
	ErrorOr<Standalone<VectorRef<DDMetricsRef>>> result = wait(
	    errorOr(brokenPromiseToNever(getShardMetricsList.getReply(GetMetricsListRequest(req.keys, req.shardLimit)))));

	if (result.isError()) {
		req.reply.sendError(result.getError());
	} else {
		GetDataDistributorMetricsReply rep;
		if (!req.midOnly) {
			rep.storageMetricsList = result.get();
		} else {
			auto& metricVec = result.get();
			if (metricVec.empty())
				rep.midShardSize = 0;
			else {
				rep.midShardSize = getMedianShardSize(metricVec.contents());
			}
		}
		req.reply.send(rep);
	}

	return Void();
}

ACTOR Future<Void> dataDistributor(DataDistributorInterface di, Reference<AsyncVar<ServerDBInfo> const> db) {
	state Reference<DataDistributorData> self(new DataDistributorData(db, di.id()));
	state Future<Void> collection = actorCollection(self->addActor.getFuture());
	state PromiseStream<GetMetricsListRequest> getShardMetricsList;
	state Database cx = openDBOnServer(db, TaskPriority::DefaultDelay, LockAware::True);
	state ActorCollection actors(false);
	state DDEnabledState ddEnabledState;
	self->addActor.send(actors.getResult());
	self->addActor.send(traceRole(Role::DATA_DISTRIBUTOR, di.id()));

	try {
		TraceEvent("DataDistributorRunning", di.id());
		self->addActor.send(waitFailureServer(di.waitFailure.getFuture()));
		self->addActor.send(cacheServerWatcher(&cx));
		state Future<Void> distributor =
		    reportErrorsExcept(dataDistribution(self, getShardMetricsList, &ddEnabledState),
		                       "DataDistribution",
		                       di.id(),
		                       &normalDataDistributorErrors());

		loop choose {
			when(wait(distributor || collection)) {
				ASSERT(false);
				throw internal_error();
			}
			when(HaltDataDistributorRequest req = waitNext(di.haltDataDistributor.getFuture())) {
				req.reply.send(Void());
				TraceEvent("DataDistributorHalted", di.id()).detail("ReqID", req.requesterID);
				break;
			}
			when(GetDataDistributorMetricsRequest req = waitNext(di.dataDistributorMetrics.getFuture())) {
				actors.add(ddGetMetrics(req, getShardMetricsList));
			}
			when(DistributorSnapRequest snapReq = waitNext(di.distributorSnapReq.getFuture())) {
				actors.add(ddSnapCreate(snapReq, db, &ddEnabledState));
			}
			when(DistributorExclusionSafetyCheckRequest exclCheckReq =
			         waitNext(di.distributorExclCheckReq.getFuture())) {
				actors.add(ddExclusionSafetyCheck(exclCheckReq, self, cx));
			}
		}
	} catch (Error& err) {
		if (normalDataDistributorErrors().count(err.code()) == 0) {
			TraceEvent("DataDistributorError", di.id()).error(err, true);
			throw err;
		}
		TraceEvent("DataDistributorDied", di.id()).error(err, true);
	}

	return Void();
}

std::unique_ptr<DDTeamCollection> testTeamCollection(int teamSize,
                                                     Reference<IReplicationPolicy> policy,
                                                     int processCount) {
	Database database = DatabaseContext::create(
	    makeReference<AsyncVar<ClientDBInfo>>(), Never(), LocalityData(), EnableLocalityLoadBalance::False);

	DatabaseConfiguration conf;
	conf.storageTeamSize = teamSize;
	conf.storagePolicy = policy;

	auto collection =
	    std::unique_ptr<DDTeamCollection>(new DDTeamCollection(database,
	                                                           UID(0, 0),
	                                                           MoveKeysLock(),
	                                                           PromiseStream<RelocateShard>(),
	                                                           makeReference<ShardsAffectedByTeamFailure>(),
	                                                           conf,
	                                                           {},
	                                                           {},
	                                                           Future<Void>(Void()),
	                                                           makeReference<AsyncVar<bool>>(true),
	                                                           IsPrimary::True,
	                                                           makeReference<AsyncVar<bool>>(false),
	                                                           PromiseStream<GetMetricsRequest>(),
	                                                           Promise<UID>(),
	                                                           PromiseStream<Promise<int>>()));

	for (int id = 1; id <= processCount; ++id) {
		UID uid(id, 0);
		StorageServerInterface interface;
		interface.uniqueID = uid;
		interface.locality.set(LiteralStringRef("machineid"), Standalone<StringRef>(std::to_string(id)));
		interface.locality.set(LiteralStringRef("zoneid"), Standalone<StringRef>(std::to_string(id % 5)));
		interface.locality.set(LiteralStringRef("data_hall"), Standalone<StringRef>(std::to_string(id % 3)));
		collection->server_info[uid] = makeReference<TCServerInfo>(
		    interface, collection.get(), ProcessClass(), true, collection->storageServerSet);
		collection->server_status.set(uid, ServerStatus(false, false, false, interface.locality));
		collection->checkAndCreateMachine(collection->server_info[uid]);
	}

	return collection;
}

std::unique_ptr<DDTeamCollection> testMachineTeamCollection(int teamSize,
                                                            Reference<IReplicationPolicy> policy,
                                                            int processCount) {
	Database database = DatabaseContext::create(
	    makeReference<AsyncVar<ClientDBInfo>>(), Never(), LocalityData(), EnableLocalityLoadBalance::False);

	DatabaseConfiguration conf;
	conf.storageTeamSize = teamSize;
	conf.storagePolicy = policy;

	auto collection =
	    std::unique_ptr<DDTeamCollection>(new DDTeamCollection(database,
	                                                           UID(0, 0),
	                                                           MoveKeysLock(),
	                                                           PromiseStream<RelocateShard>(),
	                                                           makeReference<ShardsAffectedByTeamFailure>(),
	                                                           conf,
	                                                           {},
	                                                           {},
	                                                           Future<Void>(Void()),
	                                                           makeReference<AsyncVar<bool>>(true),
	                                                           IsPrimary::True,
	                                                           makeReference<AsyncVar<bool>>(false),
	                                                           PromiseStream<GetMetricsRequest>(),
	                                                           Promise<UID>(),
	                                                           PromiseStream<Promise<int>>()));

	for (int id = 1; id <= processCount; id++) {
		UID uid(id, 0);
		StorageServerInterface interface;
		interface.uniqueID = uid;
		int process_id = id;
		int dc_id = process_id / 1000;
		int data_hall_id = process_id / 100;
		int zone_id = process_id / 10;
		int machine_id = process_id / 5;

		printf("testMachineTeamCollection: process_id:%d zone_id:%d machine_id:%d ip_addr:%s\n",
		       process_id,
		       zone_id,
		       machine_id,
		       interface.address().toString().c_str());
		interface.locality.set(LiteralStringRef("processid"), Standalone<StringRef>(std::to_string(process_id)));
		interface.locality.set(LiteralStringRef("machineid"), Standalone<StringRef>(std::to_string(machine_id)));
		interface.locality.set(LiteralStringRef("zoneid"), Standalone<StringRef>(std::to_string(zone_id)));
		interface.locality.set(LiteralStringRef("data_hall"), Standalone<StringRef>(std::to_string(data_hall_id)));
		interface.locality.set(LiteralStringRef("dcid"), Standalone<StringRef>(std::to_string(dc_id)));
		collection->server_info[uid] = makeReference<TCServerInfo>(
		    interface, collection.get(), ProcessClass(), true, collection->storageServerSet);

		collection->server_status.set(uid, ServerStatus(false, false, false, interface.locality));
	}

	int totalServerIndex = collection->constructMachinesFromServers();
	printf("testMachineTeamCollection: construct machines for %d servers\n", totalServerIndex);

	return collection;
}

TEST_CASE("DataDistribution/AddTeamsBestOf/UseMachineID") {
	wait(Future<Void>(Void()));

	int teamSize = 3; // replication size
	int processSize = 60;
	int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;

	Reference<IReplicationPolicy> policy = Reference<IReplicationPolicy>(
	    new PolicyAcross(teamSize, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state std::unique_ptr<DDTeamCollection> collection = testMachineTeamCollection(teamSize, policy, processSize);

	collection->addTeamsBestOf(30, desiredTeams, maxTeams);

	ASSERT(collection->sanityCheckTeams() == true);

	return Void();
}

TEST_CASE("DataDistribution/AddTeamsBestOf/NotUseMachineID") {
	wait(Future<Void>(Void()));

	int teamSize = 3; // replication size
	int processSize = 60;
	int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;

	Reference<IReplicationPolicy> policy = Reference<IReplicationPolicy>(
	    new PolicyAcross(teamSize, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state std::unique_ptr<DDTeamCollection> collection = testMachineTeamCollection(teamSize, policy, processSize);

	if (collection == nullptr) {
		fprintf(stderr, "collection is null\n");
		return Void();
	}

	collection->addBestMachineTeams(30); // Create machine teams to help debug
	collection->addTeamsBestOf(30, desiredTeams, maxTeams);
	collection->sanityCheckTeams(); // Server team may happen to be on the same machine team, although unlikely

	return Void();
}

TEST_CASE("DataDistribution/AddAllTeams/isExhaustive") {
	Reference<IReplicationPolicy> policy =
	    Reference<IReplicationPolicy>(new PolicyAcross(3, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state int processSize = 10;
	state int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	state int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;
	state std::unique_ptr<DDTeamCollection> collection = testTeamCollection(3, policy, processSize);

	int result = collection->addTeamsBestOf(200, desiredTeams, maxTeams);

	// The maximum number of available server teams without considering machine locality is 120
	// The maximum number of available server teams with machine locality constraint is 120 - 40, because
	// the 40 (5*4*2) server teams whose servers come from the same machine are invalid.
	ASSERT(result == 80);

	return Void();
}

TEST_CASE("/DataDistribution/AddAllTeams/withLimit") {
	Reference<IReplicationPolicy> policy =
	    Reference<IReplicationPolicy>(new PolicyAcross(3, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state int processSize = 10;
	state int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	state int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;

	state std::unique_ptr<DDTeamCollection> collection = testTeamCollection(3, policy, processSize);

	int result = collection->addTeamsBestOf(10, desiredTeams, maxTeams);

	ASSERT(result >= 10);

	return Void();
}

TEST_CASE("/DataDistribution/AddTeamsBestOf/SkippingBusyServers") {
	wait(Future<Void>(Void()));
	Reference<IReplicationPolicy> policy =
	    Reference<IReplicationPolicy>(new PolicyAcross(3, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state int processSize = 10;
	state int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	state int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;
	state int teamSize = 3;
	// state int targetTeamsPerServer = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * (teamSize + 1) / 2;
	state std::unique_ptr<DDTeamCollection> collection = testTeamCollection(teamSize, policy, processSize);

	collection->addTeam(std::set<UID>({ UID(1, 0), UID(2, 0), UID(3, 0) }), true);
	collection->addTeam(std::set<UID>({ UID(1, 0), UID(3, 0), UID(4, 0) }), true);

	state int result = collection->addTeamsBestOf(8, desiredTeams, maxTeams);

	ASSERT(result >= 8);

	for (auto process = collection->server_info.begin(); process != collection->server_info.end(); process++) {
		auto teamCount = process->second->teams.size();
		ASSERT(teamCount >= 1);
		// ASSERT(teamCount <= targetTeamsPerServer);
	}

	return Void();
}

// Due to the randomness in choosing the machine team and the server team from the machine team, it is possible that
// we may not find the remaining several (e.g., 1 or 2) available teams.
// It is hard to conclude what is the minimum number of  teams the addTeamsBestOf() should create in this situation.
TEST_CASE("/DataDistribution/AddTeamsBestOf/NotEnoughServers") {
	wait(Future<Void>(Void()));

	Reference<IReplicationPolicy> policy =
	    Reference<IReplicationPolicy>(new PolicyAcross(3, "zoneid", Reference<IReplicationPolicy>(new PolicyOne())));
	state int processSize = 5;
	state int desiredTeams = SERVER_KNOBS->DESIRED_TEAMS_PER_SERVER * processSize;
	state int maxTeams = SERVER_KNOBS->MAX_TEAMS_PER_SERVER * processSize;
	state int teamSize = 3;
	state std::unique_ptr<DDTeamCollection> collection = testTeamCollection(teamSize, policy, processSize);

	collection->addTeam(std::set<UID>({ UID(1, 0), UID(2, 0), UID(3, 0) }), true);
	collection->addTeam(std::set<UID>({ UID(1, 0), UID(3, 0), UID(4, 0) }), true);

	collection->addBestMachineTeams(10);
	int result = collection->addTeamsBestOf(10, desiredTeams, maxTeams);

	if (collection->machineTeams.size() != 10 || result != 8) {
		collection->traceAllInfo(true); // Debug message
	}

	// NOTE: Due to the pure randomness in selecting a machine for a machine team,
	// we cannot guarantee that all machine teams are created.
	// When we chnage the selectReplicas function to achieve such guarantee, we can enable the following ASSERT
	ASSERT(collection->machineTeams.size() == 10); // Should create all machine teams

	// We need to guarantee a server always have at least a team so that the server can participate in data distribution
	for (auto process = collection->server_info.begin(); process != collection->server_info.end(); process++) {
		auto teamCount = process->second->teams.size();
		ASSERT(teamCount >= 1);
	}

	// If we find all available teams, result will be 8 because we prebuild 2 teams
	ASSERT(result == 8);

	return Void();
}
