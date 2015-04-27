// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "TwitchHype.h"
#include "UnrealTournament.h"
#include "UTMutator.h"

#include "TwitchHypeMutator.generated.h"

UCLASS(Meta = (ChildCanTick))
class ATwitchHypeMutator : public AUTMutator
{
	GENERATED_UCLASS_BODY()
	
	FTwitchHype* TwitchHype;

public:
	void PostPlayerInit_Implementation(AController* C) override;
	void NotifyMatchStateChange_Implementation(FName NewState) override;
	void ScoreKill_Implementation(AController* Killer, AController* Other, TSubclassOf<UDamageType> DamageType) override;
};