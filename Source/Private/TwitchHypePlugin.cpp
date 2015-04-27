// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "TwitchHype.h"

#include "Core.h"
#include "Engine.h"
#include "ModuleManager.h"
#include "ModuleInterface.h"
#include "Engine/World.h"

IMPLEMENT_MODULE(FTwitchHypePlugin, TwitchHype)

void FTwitchHypePlugin::StartupModule()
{
	// Make an object that ticks
	TwitchHype = new FTwitchHype();

	FWorldDelegates::FWorldInitializationEvent::FDelegate OnWorldCreatedDelegate = FWorldDelegates::FWorldInitializationEvent::FDelegate::CreateRaw(TwitchHype, &FTwitchHype::OnWorldCreated);
	FDelegateHandle OnWorldCreatedDelegateHandle = FWorldDelegates::OnPostWorldInitialization.Add(OnWorldCreatedDelegate);

	FWorldDelegates::FWorldEvent::FDelegate OnWorldDestroyedDelegate = FWorldDelegates::FWorldEvent::FDelegate::CreateRaw(TwitchHype, &FTwitchHype::OnWorldDestroyed);
	FDelegateHandle OnWorldDestroyedDelegateHandle = FWorldDelegates::OnPreWorldFinishDestroy.Add(OnWorldDestroyedDelegate);
}


void FTwitchHypePlugin::ShutdownModule()
{
	// Run the destructor
	delete TwitchHype;
}