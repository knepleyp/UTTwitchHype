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
	double TopTenCooldownTime;

	UPROPERTY(config)
	bool bPrintBetConfirmations;

	UPROPERTY(config)
	float EventDelayTime;

	UPROPERTY(config)
	bool bAutoConnect;

	UPROPERTY(config)
	int32 InitialCredits;

	UPROPERTY(config)
	int32 MaxBet;

	UPROPERTY(config)
	bool bDebug;

	UPROPERTY(config)
	int32 ChatCost;

	UPROPERTY(config)
	float BettingCloseDelayTime;
};

struct FDelayedEvent
{
	FString EventType;
	FString Winner;
	float TimeLeft;
};

struct FUserProfile
{
	int32 credits;
	int32 bankrupts;

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

	bool bAutoConnect;
	bool bAuthenticated;
	double AuthenticatedTime;
	double JoinedTime;
	double LastTop10Time;
	double Top10CooldownTime;
	int32 MaxBet;
	bool bJoinedChannel;
	bool bAnnounced;
	FString ChannelName;
	FString BotNickname;
	FString OAuth;
	int32 InitialCredits;
	float EventDelayTime;
	float BettingCloseDelayTime;
	bool bDebug;
	int32 ChatCost;

	bool bPrintBetConfirmations;

	sqlite3 *db;
	TMap<FString, FUserProfile> InMemoryProfiles;
	TArray<FString> ActivePlayers;
	TArray<FDelayedEvent> DelayedEvents;
	bool bBettingOpen;

	bool bFirstBlood;
	bool bFirstSuicide;

	TMap<FString, FActiveBet> ActiveBets;
	TMap<FString, FActiveBet> ActiveFirstBloodBets;
	TMap<FString, FActiveBet> ActiveFirstSuicideBets;
	
	void OnPrivMsg(IRCMessage message);

	void PostPlayerInit(UWorld* World, AUTGameMode* GM, AController* C);
	void NotifyMatchStateChange(UWorld* World, AUTGameMode* GM, FName NewState);
	void ScoreKill(UWorld* World, AUTGameMode* GM, AController* Killer, AController* Other, TSubclassOf<UDamageType> DamageType);

	void ForgiveBets();
	void FlushToDB();

	void ParseABet(const TArray<FString>& ParsedCommand, FUserProfile* Profile, const FString& Username, TMap<FString, FActiveBet>& BetMap);

	void AwardBets(const FString& Winner, int32& MoneyWon, int32& HouseTake, TMap<FString, FActiveBet>& BetMap);

	void UndoBets(FUserProfile* Profile, const FString& Username);
	
	void SendChat(const FString& Command, FUserProfile* Profile, const FString& Username);

	void PrintTop10();
	void GiveExtraMoney(FUserProfile* Profile, const FString& Username);

	void ConnectToIRC();

	bool HasActiveBets(const FString& Username);
};

class FTwitchHypePlugin : public IModuleInterface
{
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
public:
	FTwitchHype* TwitchHype;
};