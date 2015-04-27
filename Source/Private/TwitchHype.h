// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Core.h"
#include "UnrealTournament.h"
#include "IRCClient.h"
#include "sqlite3.h"
#include "TwitchHype.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUTTwitchHype, Log, All);

// Abuse this class for config cache use
UCLASS(Blueprintable, Meta = (ChildCanTick), Config = TwitchHype)
class ATwitchHype : public AActor
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(config)
	FString OAuth;

	UPROPERTY(config)
	FString BotNickname;

	UPROPERTY(config)
	FString ChannelName;

	UPROPERTY(config)
	bool bPrintBetConfirmations;

	UPROPERTY(config)
	double Top10CooldownTime;
};

struct FUserProfile
{
	int32 credits;

	double lastusetime;
};

struct FActiveBet
{
	FString winner;

	int32 amount;

	float odds;
};

struct FTwitchHype : FTickableGameObject, FSelfRegisteringExec
{
	FTwitchHype();
	~FTwitchHype();
	virtual void Tick(float DeltaTime);
	virtual bool IsTickable() const { return true; }
	virtual bool IsTickableInEditor() const { return true; }

	// Put a real stat id here
	virtual TStatId GetStatId() const
	{
		return TStatId();
	}

	/** FSelfRegisteringExec implementation */
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override;

	void OnWorldCreated(UWorld* World, const UWorld::InitializationValues IVS);
	void OnWorldDestroyed(UWorld* World);

	TArray<UWorld*> KnownWorlds;

	IRCClient client;

	bool bAuthenticated;
	double AuthenticatedTime;
	double JoinedTime;
	double LastTop10Time;
	double Top10CooldownTime;
	bool bJoinedChannel;
	bool bAnnounced;
	FString ChannelName;
	FString BotNickname;
	FString OAuth;

	bool bPrintBetConfirmations;

	sqlite3 *db;
	TMap<FString, FUserProfile> InMemoryProfiles;
	TMap<FString, FActiveBet> ActiveBets;
	TMap<FString, FActiveBet> ActiveFirstBloodBets;
	TArray<FString> ActivePlayers;
	bool bFirstBlood;
	bool bBettingOpen;

	void OnPrivMsg(IRCMessage message);

	void PostPlayerInit(UWorld* World, AUTGameMode* GM, AController* C);
	void NotifyMatchStateChange(UWorld* World, AUTGameMode* GM, FName NewState);
	void ScoreKill(UWorld* World, AUTGameMode* GM, AController* Killer, AController* Other, TSubclassOf<UDamageType> DamageType);

	void ForgiveBets();
	void AwardBets(const FString& Winner, int32& MoneyWon, int32& HouseTake);
	void AwardFirstBloodBets(const FString& Winner, int32& MoneyWon, int32& HouseTake);
	void FlushToDB();

	void ParseBet(const TArray<FString>& ParsedCommand, FUserProfile* Profile, const FString& Username);
	void ParseFirstBloodBet(const TArray<FString>& ParsedCommand, FUserProfile* Profile, const FString& Username);

	void PrintTop10();
};

class FTwitchHypePlugin : public IModuleInterface
{
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
public:
	FTwitchHype* TwitchHype;
};