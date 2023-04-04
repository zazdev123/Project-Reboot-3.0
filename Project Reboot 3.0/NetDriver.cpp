#include "NetDriver.h"

#include "reboot.h"
#include "Actor.h"
#include "NetConnection.h"
#include "FortPlayerControllerAthena.h"
#include "GameplayStatics.h"
#include "KismetMathLibrary.h"
#include <random>

FNetworkObjectList& UNetDriver::GetNetworkObjectList()
{
	return *(*(TSharedPtr<FNetworkObjectList>*)(__int64(this) + Offsets::NetworkObjectList));
}

void UNetDriver::RemoveNetworkActor(AActor* Actor)
{
	GetNetworkObjectList().Remove(Actor);

	// RenamedStartupActors.Remove(Actor->GetFName());
}

void UNetDriver::TickFlushHook(UNetDriver* NetDriver)
{
	static auto ReplicationDriverOffset = NetDriver->GetOffset("ReplicationDriver", false);

	if (ReplicationDriverOffset == -1)
	{
		NetDriver->ServerReplicateActors();
	}
	else
	{
		if (auto ReplicationDriver = NetDriver->Get(ReplicationDriverOffset))
			reinterpret_cast<void(*)(UObject*)>(ReplicationDriver->VFTable[Offsets::ServerReplicateActors])(ReplicationDriver);
	}

	return TickFlushOriginal(NetDriver);
}

int32 ServerReplicateActors_PrepConnections(UNetDriver* NetDriver)
{
	auto& ClientConnections = NetDriver->GetClientConnections();

	int32 NumClientsToTick = ClientConnections.Num();

	bool bFoundReadyConnection = false;

	for (int32 ConnIdx = 0; ConnIdx < ClientConnections.Num(); ConnIdx++)
	{
		UNetConnection* Connection = ClientConnections.at(ConnIdx);
		if (!Connection) continue;
		// check(Connection->State == USOCK_Pending || Connection->State == USOCK_Open || Connection->State == USOCK_Closed);
		// checkSlow(Connection->GetUChildConnection() == NULL);

		AActor* OwningActor = Connection->GetOwningActor();

		if (OwningActor != NULL) // && /* Connection->State == USOCK_Open && */ (Connection->Driver->Time - Connection->LastReceiveTime < 1.5f))
		{
			bFoundReadyConnection = true;

			AActor* DesiredViewTarget = OwningActor;

			if (Connection->GetPlayerController())
			{
				if (AActor* ViewTarget = Connection->GetPlayerController()->GetViewTarget())
				{
					DesiredViewTarget = ViewTarget;
				}
			}

			Connection->GetViewTarget() = DesiredViewTarget;
		}
		else
		{
			Connection->GetViewTarget() = NULL;
		}
	}

	return bFoundReadyConnection ? NumClientsToTick : 0;
}

enum class ENetRole : uint8_t
{
	ROLE_None = 0,
	ROLE_SimulatedProxy = 1,
	ROLE_AutonomousProxy = 2,
	ROLE_Authority = 3,
	ROLE_MAX = 4
};

enum class ENetDormancy : uint8_t
{
	DORM_Never = 0,
	DORM_Awake = 1,
	DORM_DormantAll = 2,
	DORM_DormantPartial = 3,
	DORM_Initial = 4,
	DORN_MAX = 5,
	ENetDormancy_MAX = 6
};

FORCEINLINE float FRand()
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<> dis(0, 1);
	float random_number = dis(gen);

	return random_number;
}

#define USEOBJECTLIST

void UNetDriver::ServerReplicateActors_BuildConsiderList(std::vector<FNetworkObjectInfo*>& OutConsiderList)
{
	std::vector<AActor*> ActorsToRemove;

#ifdef USEOBJECTLIST
	auto& ActiveObjects = GetNetworkObjectList().ActiveNetworkObjects;
#else
	TArray<AActor*> Actors = UGameplayStatics::GetAllActorsOfClass(GetWorld(), AActor::StaticClass());
#endif

	auto World = GetWorld();

#ifdef USEOBJECTLIST
	// for (int i = 0; i < ActiveObjects.Elements.Num(); i++)
	for (const TSharedPtr<FNetworkObjectInfo>& ActorInfo : ActiveObjects)
	{
		// auto& ActorInfo = ActiveObjects.Elements.Data.at(i).ElementData.Value;

		if (!ActorInfo->bPendingNetUpdate && UGameplayStatics::GetTimeSeconds(GetWorld()) <= ActorInfo->NextUpdateTime)
		{
			continue;
		}

		// if (IsBadReadPtr(ActorInfo, 8))
			// continue;

		auto Actor = ActorInfo->Actor;

#else

	for (int i = 0; i < Actors.Num(); i++)
	{
		auto Actor = Actors.at(i);

#endif

		if (!Actor)
			continue;

		if (Actor->IsPendingKillPending())
		// if (Actor->IsPendingKill())
		{
			ActorsToRemove.push_back(Actor);
			continue;
		}

		static auto RemoteRoleOffset = Actor->GetOffset("RemoteRole");

		if (Actor->Get<ENetRole>(RemoteRoleOffset) == ENetRole::ROLE_None)
		{
			ActorsToRemove.push_back(Actor);
			continue;
		}

		// We should add a NetDriverName check but I don't believe it is needed.

		// We should check if the actor is initialized here.

		// We should check the level stuff here.

		static auto NetDormancyOffset = Actor->GetOffset("NetDormancy");

		if (Actor->Get<ENetDormancy>(NetDormancyOffset) == ENetDormancy::DORM_Initial && Actor->IsNetStartupActor()) // IsDormInitialStartupActor
		{
			continue;
		}

		// We should check NeedsLoadForClient here.
		// We should make sure the actor is in the same world here but I don't believe it is needed.

#ifndef USEOBJECTLIST
		FNetworkObjectInfo* ActorInfo = new FNetworkObjectInfo;
		ActorInfo->Actor = Actor;
#else
		auto TimeSeconds = UGameplayStatics::GetTimeSeconds(World); // Can we do this outside of the loop?

		if (ActorInfo->LastNetReplicateTime == 0)
		{
			ActorInfo->LastNetReplicateTime = UGameplayStatics::GetTimeSeconds(World);
			ActorInfo->OptimalNetUpdateDelta = 1.0f / Actor->GetNetUpdateFrequency();
		}

		const float ScaleDownStartTime = 2.0f;
		const float ScaleDownTimeRange = 5.0f;

		const float LastReplicateDelta = TimeSeconds - ActorInfo->LastNetReplicateTime;

		if (LastReplicateDelta > ScaleDownStartTime)
		{
			static auto MinNetUpdateFrequencyOffset = Actor->GetOffset("MinNetUpdateFrequency");

			if (Actor->Get<float>(MinNetUpdateFrequencyOffset) == 0.0f)
			{
				Actor->Get<float>(MinNetUpdateFrequencyOffset) = 2.0f;
			}

			const float MinOptimalDelta = 1.0f / Actor->GetNetUpdateFrequency();									  // Don't go faster than NetUpdateFrequency
			const float MaxOptimalDelta = max(1.0f / Actor->GetNetUpdateFrequency(), MinOptimalDelta); // Don't go slower than MinNetUpdateFrequency (or NetUpdateFrequency if it's slower)

			const float Alpha = std::clamp((LastReplicateDelta - ScaleDownStartTime) / ScaleDownTimeRange, 0.0f, 1.0f); // should we use fmath?
			ActorInfo->OptimalNetUpdateDelta = std::lerp(MinOptimalDelta, MaxOptimalDelta, Alpha); // should we use fmath?
		}

		if (!ActorInfo->bPendingNetUpdate)
		{
			constexpr bool bUseAdapativeNetFrequency = false;
			const float NextUpdateDelta = bUseAdapativeNetFrequency ? ActorInfo->OptimalNetUpdateDelta : 1.0f / Actor->GetNetUpdateFrequency();

			// then set the next update time
			float ServerTickTime = 1.f / 30;
			ActorInfo->NextUpdateTime = TimeSeconds + FRand() * ServerTickTime + NextUpdateDelta;
			static auto TimeOffset = GetOffset("Time");
			ActorInfo->LastNetUpdateTime = Get<float>(TimeOffset);
		}

		ActorInfo->bPendingNetUpdate = false;
#endif

		OutConsiderList.push_back(ActorInfo.Get());

		static void (*CallPreReplication)(AActor*, UNetDriver*) = decltype(CallPreReplication)(Addresses::CallPreReplication);
		CallPreReplication(Actor, this);
	}

#ifndef USEOBJECTLIST
	Actors.Free();
#else
	for (auto Actor : ActorsToRemove)
	{
		if (!Actor)
			continue;

		/* LOG_INFO(LogDev, "Removing actor: {}", Actor ? Actor->GetFullName() : "InvalidObject");
		RemoveNetworkActor(Actor);
		LOG_INFO(LogDev, "Finished removing actor."); */
	}
#endif
}

using UChannel = UObject;
using UActorChannel = UObject;

static UActorChannel* FindChannel(AActor* Actor, UNetConnection* Connection)
{
	static auto OpenChannelsOffset = Connection->GetOffset("OpenChannels");
	auto& OpenChannels = Connection->Get<TArray<UChannel*>>(OpenChannelsOffset);

	static auto ActorChannelClass = FindObject<UClass>("/Script/Engine.ActorChannel");

	// LOG_INFO(LogReplication, "OpenChannels.Num(): {}", OpenChannels.Num());

	for (int i = 0; i < OpenChannels.Num(); i++)
	{
		auto Channel = OpenChannels.at(i);

		if (!Channel)
			continue;

		// LOG_INFO(LogReplication, "[{}] Class {}", i, Channel->ClassPrivate ? Channel->ClassPrivate->GetFullName() : "InvalidObject");

		if (!Channel->IsA(ActorChannelClass)) // (Channel->ClassPrivate == ActorChannelClass)
			continue;

		static auto ActorOffset = Channel->GetOffset("Actor");
		auto ChannelActor = Channel->Get<AActor*>(ActorOffset);

		// LOG_INFO(LogReplication, "[{}] {}", i, ChannelActor->GetFullName());

		if (ChannelActor != Actor)
			continue;
		
		return (UActorChannel*)Channel;
	}

	return NULL;
}

struct FNetViewer
{
	UNetConnection* Connection;                                               // 0x0000(0x0008) (ZeroConstructor, IsPlainOldData)
	AActor* InViewer;                                                 // 0x0008(0x0008) (ZeroConstructor, IsPlainOldData)
	AActor* ViewTarget;                                               // 0x0010(0x0008) (ZeroConstructor, IsPlainOldData)
	FVector                                     ViewLocation;                                             // 0x0018(0x000C) (IsPlainOldData)
	FVector                                     ViewDir;
};

static bool IsActorRelevantToConnection(AActor* Actor, std::vector<FNetViewer>& ConnectionViewers)
{
	for (int32 viewerIdx = 0; viewerIdx < ConnectionViewers.size(); viewerIdx++)
	{
		if (!ConnectionViewers[viewerIdx].ViewTarget)
			continue;

		// static bool (*IsNetRelevantFor)(AActor*, AActor*, AActor*, FVector&) = decltype(IsNetRelevantFor)(__int64(GetModuleHandleW(0)) + 0x1ECC700);

		static auto index = Offsets::IsNetRelevantFor;

		// if (Actor->IsNetRelevantFor(ConnectionViewers[viewerIdx].InViewer, ConnectionViewers[viewerIdx].ViewTarget, ConnectionViewers[viewerIdx].ViewLocation))
		// if (IsNetRelevantFor(Actor, ConnectionViewers[viewerIdx].InViewer, ConnectionViewers[viewerIdx].ViewTarget, ConnectionViewers[viewerIdx].ViewLocation))
		if (reinterpret_cast<bool(*)(AActor*, AActor*, AActor*, FVector&)>(Actor->VFTable[index])(
			Actor, ConnectionViewers[viewerIdx].InViewer, ConnectionViewers[viewerIdx].ViewTarget, ConnectionViewers[viewerIdx].ViewLocation))
		{
			return true;
		}
	}

	return false;
}

static FNetViewer ConstructNetViewer(UNetConnection* NetConnection)
{
	FNetViewer newViewer{};
	newViewer.Connection = NetConnection;
	newViewer.InViewer = NetConnection->GetPlayerController() ? NetConnection->GetPlayerController() : NetConnection->GetOwningActor();
	newViewer.ViewTarget = NetConnection->GetViewTarget();

	if (!NetConnection->GetOwningActor() || !(!NetConnection->GetPlayerController() || (NetConnection->GetPlayerController() == NetConnection->GetOwningActor())))
		return newViewer;

	APlayerController* ViewingController = NetConnection->GetPlayerController();

	newViewer.ViewLocation = newViewer.ViewTarget->GetActorLocation();

	if (ViewingController)
	{
		FRotator ViewRotation = ViewingController->GetControlRotation();
		AFortPlayerControllerAthena::GetPlayerViewPointHook(Cast<AFortPlayerControllerAthena>(ViewingController, false), newViewer.ViewLocation, ViewRotation);
		newViewer.ViewDir = ViewRotation.Vector();
	}

	return newViewer;
}

int32 UNetDriver::ServerReplicateActors()
{
	int32 Updated = 0;

	++(*(int*)(__int64(this) + Offsets::ReplicationFrame));

	const int32 NumClientsToTick = ServerReplicateActors_PrepConnections(this);

	if (NumClientsToTick == 0)
	{
		// No connections are ready this frame
		return 0;
	}

	// AFortWorldSettings* WorldSettings = GetFortWorldSettings(NetDriver->World);

	// bool bCPUSaturated = false;
	float ServerTickTime = 30.f; // Globals::MaxTickRate; // GEngine->GetMaxTickRate(DeltaSeconds);
	/* if (ServerTickTime == 0.f)
	{
		ServerTickTime = DeltaSeconds;
	}
	else */
	{
		ServerTickTime = 1.f / ServerTickTime;
		// bCPUSaturated = DeltaSeconds > 1.2f * ServerTickTime;
	}

	std::vector<FNetworkObjectInfo*> ConsiderList;

#ifdef USEOBJECTLIST
	ConsiderList.reserve(GetNetworkObjectList().ActiveNetworkObjects.Num());
#endif

	// std::cout << "ConsiderList.size(): " << GetNetworkObjectList(NetDriver).ActiveNetworkObjects.Num() << '\n';

	auto World = GetWorld();

	ServerReplicateActors_BuildConsiderList(ConsiderList);

	for (int32 i = 0; i < this->GetClientConnections().Num(); i++)
	{
		UNetConnection* Connection = this->GetClientConnections().at(i);

		if (!Connection)
			continue;

		if (i >= NumClientsToTick)
			continue;

		if (!Connection->GetViewTarget())
			continue;

		if (Connection->GetPlayerController())
		{
			static void (*SendClientAdjustment)(APlayerController*) = decltype(SendClientAdjustment)(Addresses::SendClientAdjustment);
			SendClientAdjustment(Connection->GetPlayerController());
		}

		for (auto& ActorInfo : ConsiderList)
		{
			if (!ActorInfo || !ActorInfo->Actor)
				continue;

			auto Actor = ActorInfo->Actor;

			auto Channel = FindChannel(Actor, Connection);

			if (Addresses::ActorChannelClose && Offsets::IsNetRelevantFor)
			{
				static void (*ActorChannelClose)(UActorChannel*) = decltype(ActorChannelClose)(Addresses::ActorChannelClose);

				std::vector<FNetViewer> ConnectionViewers;
				ConnectionViewers.push_back(ConstructNetViewer(Connection));

				if (!Actor->IsAlwaysRelevant() && !Actor->UsesOwnerRelevancy() && !Actor->IsOnlyRelevantToOwner())
				{
					if (Connection && Connection->GetViewTarget())
					{
						auto Viewer = Connection->GetViewTarget();
						auto Loc = Viewer->GetActorLocation();

						if (!IsActorRelevantToConnection(Actor, ConnectionViewers))
						{
							if (Channel)
								ActorChannelClose(Channel);

							continue;
						}
					}
				}
			}

			static UChannel* (*CreateChannel)(UNetConnection*, int, bool, int32_t) = decltype(CreateChannel)(Addresses::CreateChannel);
			static __int64 (*ReplicateActor)(UActorChannel*) = decltype(ReplicateActor)(Addresses::ReplicateActor);
			static __int64 (*SetChannelActor)(UActorChannel*, AActor*) = decltype(SetChannelActor)(Addresses::SetChannelActor);

			if (!Channel)
			{
				if (Actor->IsA(APlayerController::StaticClass()) && Actor != Connection->GetPlayerController()) // isnetrelevantfor should handle this iirc
					continue;

				Channel = (UActorChannel*)CreateChannel(Connection, 2, true, -1);

				if (Channel)
				{
					SetChannelActor(Channel, Actor);
				}

#ifdef USEOBJECTLIST
				if (Actor->GetNetUpdateFrequency() < 1.0f)
				{
					ActorInfo->NextUpdateTime = UGameplayStatics::GetTimeSeconds(GetWorld()) + 0.2f * FRand();
				}
#endif
			}

			if (Channel)
			{
				if (ReplicateActor(Channel))
				{ 
#ifdef USEOBJECTLIST
					auto TimeSeconds = UGameplayStatics::GetTimeSeconds(World);
					const float MinOptimalDelta = 1.0f / Actor->GetNetUpdateFrequency();
					const float MaxOptimalDelta = max(1.0f / Actor->GetMinNetUpdateFrequency(), MinOptimalDelta);
					const float DeltaBetweenReplications = (TimeSeconds - ActorInfo->LastNetReplicateTime);

					// Choose an optimal time, we choose 70% of the actual rate to allow frequency to go up if needed
					ActorInfo->OptimalNetUpdateDelta = std::clamp(DeltaBetweenReplications * 0.7f, MinOptimalDelta, MaxOptimalDelta); // should we use fmath?
					ActorInfo->LastNetReplicateTime = TimeSeconds;
#endif
				}
			}
		}
	}

	// shuffle the list of connections if not all connections were ticked
	/*
	if (NumClientsToTick < NetDriver->ClientConnections.Num())
	{
		int32 NumConnectionsToMove = NumClientsToTick;
		while (NumConnectionsToMove > 0)
		{
			// move all the ticked connections to the end of the list so that the other connections are considered first for the next frame
			UNetConnection* Connection = NetDriver->ClientConnections[0];
			NetDriver->ClientConnections.RemoveAt(0, 1);
			NetDriver->ClientConnections.Add(Connection);
			NumConnectionsToMove--;
		}
	}
	*/

	return Updated;
}