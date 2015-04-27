// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "TwitchHype.h"
#include "TwitchHypeMutator.h"

ATwitchHypeMutator::ATwitchHypeMutator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TwitchHype = nullptr;

	// Find the object that the plugin created
	if (FModuleManager::Get().IsModuleLoaded("TwitchHype"))
	{
		TwitchHype = FModuleManager::GetModuleChecked<FTwitchHypePlugin>("TwitchHype").TwitchHype;
	}
}

void ATwitchHypeMutator::PostPlayerInit_Implementation(AController* C)
{
	AUTGameMode* GM = GetWorld()->GetAuthGameMode<AUTGameMode>();
	if (TwitchHype != nullptr && GM != nullptr)
	{
		TwitchHype->PostPlayerInit(GetWorld(), GM, C);
	}
}

void ATwitchHypeMutator::NotifyMatchStateChange_Implementation(FName NewState)
{
	AUTGameMode* GM = GetWorld()->GetAuthGameMode<AUTGameMode>();
	if (TwitchHype != nullptr && GM != nullptr)
	{
		TwitchHype->NotifyMatchStateChange(GetWorld(), GM, NewState);
	}
}

void ATwitchHypeMutator::ScoreKill_Implementation(AController* Killer, AController* Other, TSubclassOf<UDamageType> DamageType)
{
	AUTGameMode* GM = GetWorld()->GetAuthGameMode<AUTGameMode>();
	if (TwitchHype != nullptr && GM != nullptr)
	{
		TwitchHype->ScoreKill(GetWorld(), GM, Killer, Other, DamageType);
	}
}