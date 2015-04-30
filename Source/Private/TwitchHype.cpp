// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "TwitchHype.h"
#include "UnrealTournament.h"
#include "UTPlayerController.h"
#include "UTGameState.h"
#include "Core.h"

DEFINE_LOG_CATEGORY(LogUTTwitchHype);

ATwitchHype::ATwitchHype(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bPrintBetConfirmations = false;
	TopTenCooldownTime = 60;
	EventDelayTime = 25;
	BettingCloseDelayTime = 30;
	bAutoConnect = false;
	InitialCredits = 1500;
	MaxBet = 1000;
	bDebug = false;
	ChatCost = 2000;
}

void OnPrivMsg(IRCMessage message, struct FTwitchHype* TwitchHype)
{
	TwitchHype->OnPrivMsg(message);
}

FTwitchHype::FTwitchHype()
{
	bAuthenticated = false;
	bJoinedChannel = false;
	bAnnounced = false;
	bBettingOpen = false;
	AuthenticatedTime = 0;
	db = nullptr;
	bFirstBlood = false;
	bFirstSuicide = false;
	LastTop10Time = 0;

	ATwitchHype* Settings = ATwitchHype::StaticClass()->GetDefaultObject<ATwitchHype>();
	// Load these from config file
	ChannelName = Settings->ChannelName;
	BotNickname = Settings->BotNickname;
	OAuth = Settings->OAuth;
	bPrintBetConfirmations = Settings->bPrintBetConfirmations;
	Top10CooldownTime = Settings->TopTenCooldownTime;
	EventDelayTime = Settings->EventDelayTime;
	BettingCloseDelayTime = Settings->BettingCloseDelayTime;
	bAutoConnect = Settings->bAutoConnect;
	InitialCredits = Settings->InitialCredits;
	MaxBet = Settings->MaxBet;
	bDebug = Settings->bDebug;
	ChatCost = Settings->ChatCost;

	FString DatabasePath = FPaths::GameSavedDir() / "TwitchHype.db";
	//sqlite3_open_v2(TCHAR_TO_ANSI(*DatabasePath), &db, SQLITE_OPEN_NOMUTEX, nullptr);
	if (sqlite3_open(TCHAR_TO_ANSI(*DatabasePath), &db))
	{
		UE_LOG(LogUTTwitchHype, Warning, TEXT("Could not open database"));
		sqlite3_close(db);
		db = nullptr;
	}

	if (db)
	{
		// http://www.sqlite.org/lang_createtable.html#rowid claims this is an alias for row id
		FString CreateCommand = TEXT("CREATE TABLE IF NOT EXISTS Users (name varchar(50) NOT NULL PRIMARY KEY, credits int, bankrupts int, jointime timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP)");
		sqlite3_exec(db, TCHAR_TO_ANSI(*CreateCommand), nullptr, nullptr, nullptr);
		
		FString QueryCommand = TEXT("SELECT name, credits FROM Users");
		sqlite3_stmt *sqlStatement;
		if (sqlite3_prepare_v2(db, TCHAR_TO_ANSI(*QueryCommand), -1, &sqlStatement, NULL) == SQLITE_OK)
		{
			while (sqlite3_step(sqlStatement) == SQLITE_ROW)
			{
				FUserProfile Profile;
				FString Name = FString(ANSI_TO_TCHAR((const char*)sqlite3_column_text(sqlStatement, 0)));
				Profile.credits = sqlite3_column_int(sqlStatement, 1);

				InMemoryProfiles.Add(Name, Profile);
			}
		}
		sqlite3_finalize(sqlStatement);

	}

	client.HookIRCCommand("PRIVMSG", &::OnPrivMsg, this);

	if (bAutoConnect)
	{
		ConnectToIRC();
	}
}

FTwitchHype::~FTwitchHype()
{
	if (db)
	{
		FlushToDB();

		sqlite3_close(db);
	}
}

void FTwitchHype::FlushToDB()
{
	for (auto It = InMemoryProfiles.CreateConstIterator(); It; ++It)
	{
		// mirror memory back to the database, %Q will try to escape any injection hacks
		const FUserProfile& Profile = It.Value();
		char *zSQL = sqlite3_mprintf("UPDATE Users SET credits=%d,bankrupts=%d WHERE name=%Q", Profile.credits, Profile.bankrupts, TCHAR_TO_ANSI(*It.Key()));
		sqlite3_exec(db, zSQL, 0, 0, 0);
		sqlite3_free(zSQL);
	}
}

void FTwitchHype::OnWorldCreated(UWorld* World, const UWorld::InitializationValues IVS)
{
	KnownWorlds.Add(World);
}

void FTwitchHype::OnWorldDestroyed(UWorld* World)
{
	KnownWorlds.Remove(World);
}

void FTwitchHype::ConnectToIRC()
{
	if (client.Connected())
	{
		return;
	}

	bAuthenticated = false;
	bJoinedChannel = false;
	if (bDebug)
	{
		client.Debug(true);
	}

	if (client.InitSocket())
	{
		FString host = TEXT("irc.twitch.tv");
		int32 port = 6667;
		// non-blocking connect
		if (!client.Connect(TCHAR_TO_ANSI(*host), port))
		{
		}
	}
}

bool FTwitchHype::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (FParse::Command(&Cmd, TEXT("IRCDISCONNECT")))
	{
		if (client.Connected())
		{
			client.Disconnect();
		}

		bAuthenticated = false;
		bJoinedChannel = false;

		return true;
	}

	if (FParse::Command(&Cmd, TEXT("IRCCONNECT")))
	{
		ConnectToIRC();

		return true;
	}

	if (FParse::Command(&Cmd, TEXT("FLUSHTODB")))
	{
		FlushToDB();

		return true;
	}

	if (FParse::Command(&Cmd, TEXT("FORGIVEBETS")))
	{
		ForgiveBets();
		ActivePlayers.Empty();

		return true;
	}

	return false;
}

void FTwitchHype::Tick(float DeltaTime)
{
	if (GIsEditor)
	{
		return;
	}

	if (client.Connecting())
	{
		client.CheckConnected();
	}
	else if (client.Connected())
	{
		if (!bAuthenticated)
		{
			client.Login(std::string(TCHAR_TO_ANSI(*BotNickname)), std::string(TCHAR_TO_ANSI(*OAuth)));
			bAuthenticated = true;
			AuthenticatedTime = FPlatformTime::Seconds();
		}
		else if (!bJoinedChannel && FPlatformTime::Seconds() - AuthenticatedTime > 1.0f)
		{
			FString JoinCommand = FString::Printf(TEXT("JOIN %s"), *ChannelName);
			client.SendIRC(TCHAR_TO_ANSI(*JoinCommand));
			bJoinedChannel = true;
			bAnnounced = false;
			JoinedTime = FPlatformTime::Seconds();
		}
		else if (!bAnnounced && bJoinedChannel && FPlatformTime::Seconds() - JoinedTime > 1.0f)
		{
			FString HelloMessage = FString::Printf(TEXT("PRIVMSG %s :Hello friends, your friendly UT bot is back!"), *ChannelName);
			client.SendIRC(TCHAR_TO_ANSI(*HelloMessage));
			bAnnounced = true;
		}
		client.ReceiveData();
	}
	
	for (auto Iter = DelayedEvents.CreateIterator(); Iter; ++Iter)
	{
		Iter->TimeLeft -= DeltaTime;
		if (Iter->TimeLeft <= 0)
		{
			if (Iter->EventType == TEXT("BettingClosed"))
			{
				bBettingOpen = false;
				FString InProgress = FString::Printf(TEXT("PRIVMSG %s :The match is starting, betting is now closed!"), *ChannelName);
				client.SendIRC(TCHAR_TO_ANSI(*InProgress));
			}

			if (Iter->EventType == TEXT("FirstBlood"))
			{
				FString FirstBlood = FString::Printf(TEXT("PRIVMSG %s :First Blood goes to %s!"), *ChannelName, *Iter->Winner);
				client.SendIRC(TCHAR_TO_ANSI(*FirstBlood));

				int32 MoneyWon = 0;
				int32 HouseTake = 0;
				AwardBets(Iter->Winner, MoneyWon, HouseTake, ActiveFirstBloodBets);

				FString BettingStats = FString::Printf(TEXT("PRIVMSG %s :Betting stats: %d credits paid out, %d credits lost"), *ChannelName, MoneyWon, HouseTake);
				client.SendIRC(TCHAR_TO_ANSI(*BettingStats));
			}

			if (Iter->EventType == TEXT("FirstSuicide"))
			{
				FString FirstBlood = FString::Printf(TEXT("PRIVMSG %s :First Suicide goes to %s!"), *ChannelName, *Iter->Winner);
				client.SendIRC(TCHAR_TO_ANSI(*FirstBlood));

				int32 MoneyWon = 0;
				int32 HouseTake = 0;
				AwardBets(Iter->Winner, MoneyWon, HouseTake, ActiveFirstSuicideBets);

				FString BettingStats = FString::Printf(TEXT("PRIVMSG %s :Betting stats: %d credits paid out, %d credits lost"), *ChannelName, MoneyWon, HouseTake);
				client.SendIRC(TCHAR_TO_ANSI(*BettingStats));
			}

			if (Iter->EventType == TEXT("MatchEnd"))
			{
				FString WaitingPostMatch = FString::Printf(TEXT("PRIVMSG %s :The match is over, thanks for betting!"), *ChannelName);
				client.SendIRC(TCHAR_TO_ANSI(*WaitingPostMatch));

				int32 MoneyWon = 0;
				int32 HouseTake = 0;
				AwardBets(Iter->Winner, MoneyWon, HouseTake, ActiveBets);

				FString BettingStats = FString::Printf(TEXT("PRIVMSG %s :Betting stats: %d credits paid out, %d credits lost"), *ChannelName, MoneyWon, HouseTake);
				client.SendIRC(TCHAR_TO_ANSI(*BettingStats));

				ActivePlayers.Empty();
			}

			DelayedEvents.RemoveAt(Iter.GetIndex());
		}
	}
}

void FTwitchHype::OnPrivMsg(IRCMessage message)
{	
	std::string text = message.parameters.at(message.parameters.size() - 1);
	FString Command(text.c_str());
	FString Username(message.prefix.nick.c_str());

	if (text == "!register")
	{		
		if (InMemoryProfiles.Find(Username) == nullptr)
		{
			FUserProfile Profile;
			Profile.credits = InitialCredits;
			Profile.bankrupts = 0;
			InMemoryProfiles.Add(Username, Profile);
			
			// mirror memory back to the database, %Q will try to escape any injection hacks
			char *zSQL = sqlite3_mprintf("INSERT INTO Users (name, credits, bankrupts) VALUES (%Q, %d, %d)", message.prefix.nick.c_str(), Profile.credits, 0);
			sqlite3_exec(db, zSQL, 0, 0, 0);
			sqlite3_free(zSQL);

			FString AccountCreated = FString::Printf(TEXT("PRIVMSG %s :Account created for %s!"), *ChannelName, *Username);
			client.SendIRC(TCHAR_TO_ANSI(*AccountCreated));
		}
		else
		{
			FString AccountCreated = FString::Printf(TEXT("PRIVMSG %s :Account already exists for %s!"), *ChannelName, *Username);
			client.SendIRC(TCHAR_TO_ANSI(*AccountCreated));
		}

		return;
	}

	if (text == "!credits")
	{
		FUserProfile* Profile = InMemoryProfiles.Find(Username);
		if (Profile != nullptr)
		{
			FString AccountCredits = FString::Printf(TEXT("PRIVMSG %s :%s you have %d credits."), *ChannelName, *Username, Profile->credits);
			client.SendIRC(TCHAR_TO_ANSI(*AccountCredits));

		}
		else
		{
			FString NoAccountCreated = FString::Printf(TEXT("PRIVMSG %s :No account exists for %s, please use !register"), *ChannelName, *Username);
			client.SendIRC(TCHAR_TO_ANSI(*NoAccountCreated));
		}

		return;
	}

	TArray<FString> ParsedCommand;
	Command.ParseIntoArrayWS(&ParsedCommand);
	if (ParsedCommand.Num() > 0)
	{
		FUserProfile* Profile = InMemoryProfiles.Find(Username);
		if (Profile != nullptr)
		{
			if (ParsedCommand[0] == TEXT("!bet"))
			{
				ParseABet(ParsedCommand, Profile, Username, ActiveBets);
			}
			else if (ParsedCommand[0] == TEXT("!firstbloodbet"))
			{
				ParseABet(ParsedCommand, Profile, Username, ActiveFirstBloodBets);
			}
			else if (ParsedCommand[0] == TEXT("!firstsuicidebet"))
			{
				ParseABet(ParsedCommand, Profile, Username, ActiveFirstSuicideBets);
			}
			else if (ParsedCommand[0] == TEXT("!top10"))
			{
				PrintTop10();
			}
			else if (ParsedCommand[0] == TEXT("!bankrupt"))
			{
				GiveExtraMoney(Profile, Username);
			}
			else if (ParsedCommand[0] == TEXT("!undobets"))
			{
				UndoBets(Profile, Username);
			}
			else if (ParsedCommand[0] == TEXT("!chat"))
			{
				SendChat(Command, Profile, Username);
			}
		}
		else if (ParsedCommand[0][0] == TEXT('!'))
		{
			FString NoAccountCreated = FString::Printf(TEXT("PRIVMSG %s :No account exists for %s, please use !register"), *ChannelName, *Username);
			client.SendIRC(TCHAR_TO_ANSI(*NoAccountCreated));
		}
	}
	/*
	//@debug
	for (int32 i = 0; i < KnownWorlds.Num(); i++)
	{
		AUTPlayerController* UTPC = Cast<AUTPlayerController>(GEngine->GetFirstLocalPlayerController(KnownWorlds[i]));
		if (UTPC)
		{
			UTPC->ServerSay(ANSI_TO_TCHAR(text.c_str()), false);
		}
	}*/
}

void FTwitchHype::PostPlayerInit(UWorld* World, AUTGameMode* GM, AController* C)
{
	if (C != nullptr && C->PlayerState != nullptr && !C->PlayerState->bOnlySpectator)
	{
		FString PlayerJoined = FString::Printf(TEXT("PRIVMSG %s :%s has joined the game!"), *ChannelName, *C->PlayerState->PlayerName);
		client.SendIRC(TCHAR_TO_ANSI(*PlayerJoined));
		ActivePlayers.Add(C->PlayerState->PlayerName);
	}
}

void FTwitchHype::NotifyMatchStateChange(UWorld* World, AUTGameMode* GM, FName NewState)
{
	if (NewState == MatchState::EnteringMap)
	{
		FString EnteringMap = FString::Printf(TEXT("PRIVMSG %s :We've started %s map!"), *ChannelName, *World->GetMapName());
		client.SendIRC(TCHAR_TO_ANSI(*EnteringMap));
		ActivePlayers.Empty();
		bBettingOpen = true;
	}
	else if (NewState == MatchState::WaitingPostMatch)
	{
		if (GM && GM->UTGameState && GM->UTGameState->WinnerPlayerState)
		{
			FDelayedEvent WinEvent;
			WinEvent.EventType = TEXT("MatchEnd");
			WinEvent.Winner = GM->UTGameState->WinnerPlayerState->PlayerName;
			WinEvent.TimeLeft = EventDelayTime;

			DelayedEvents.Add(WinEvent);
		}
		else
		{
			ForgiveBets();
		}
	}
	else if (NewState == MatchState::InProgress)
	{
		FDelayedEvent BettingClosedEvent;
		BettingClosedEvent.EventType = TEXT("BettingClosed");
		BettingClosedEvent.TimeLeft = BettingCloseDelayTime;

		DelayedEvents.Add(BettingClosedEvent);
		
		bFirstBlood = false;
		bFirstSuicide = false;
	}
	else if (NewState == MatchState::Aborted)
	{
		FString Aborted = FString::Printf(TEXT("PRIVMSG %s :The match was aborted, active bets are forgiven!"), *ChannelName);
		client.SendIRC(TCHAR_TO_ANSI(*Aborted));

		ForgiveBets();
	}
	else if (NewState == MatchState::WaitingToStart)
	{
		FString WaitingToStart = FString::Printf(TEXT("PRIVMSG %s :The match is waiting to start on %s!"), *ChannelName, *World->GetMapName());
		client.SendIRC(TCHAR_TO_ANSI(*WaitingToStart));
		bBettingOpen = true;
	}
	// Not exposed yet due to missing UNREALTOURNAMENT_API
	/*
	else if (NewState == MatchState::CountdownToBegin)
	{
		client.SendIRC("PRIVMSG #petenub :We've entered a new map!");
	}
	*/
}

void FTwitchHype::ScoreKill(UWorld* World, AUTGameMode* GM, AController* Killer, AController* Other, TSubclassOf<UDamageType> DamageType)
{
	if (!bFirstBlood && Killer != Other)
	{
		bFirstBlood = true;
		if (Killer && Killer->PlayerState)
		{
			FDelayedEvent FirstBloodEvent;
			FirstBloodEvent.EventType = TEXT("FirstBlood");
			FirstBloodEvent.Winner = Killer->PlayerState->PlayerName;
			FirstBloodEvent.TimeLeft = EventDelayTime;

			DelayedEvents.Add(FirstBloodEvent);
		}
	}

	if (!bFirstSuicide && Killer == Other)
	{
		bFirstSuicide = true;
		if (Killer && Killer->PlayerState)
		{
			FDelayedEvent FirstSuicideEvent;
			FirstSuicideEvent.EventType = TEXT("FirstSuicide");
			FirstSuicideEvent.Winner = Killer->PlayerState->PlayerName;
			FirstSuicideEvent.TimeLeft = EventDelayTime;

			DelayedEvents.Add(FirstSuicideEvent);
		}
	}
}

void FTwitchHype::ForgiveBets()
{
	for (auto It = ActiveBets.CreateConstIterator(); It; ++It)
	{
		InMemoryProfiles[It.Key()].credits += It.Value().amount;
	}
	ActiveBets.Empty();

	for (auto It = ActiveFirstBloodBets.CreateConstIterator(); It; ++It)
	{
		InMemoryProfiles[It.Key()].credits += It.Value().amount;
	}
	ActiveFirstBloodBets.Empty();

	for (auto It = ActiveFirstSuicideBets.CreateConstIterator(); It; ++It)
	{
		InMemoryProfiles[It.Key()].credits += It.Value().amount;
	}
	ActiveFirstSuicideBets.Empty();
}

void FTwitchHype::AwardBets(const FString& Winner, int32& MoneyWon, int32& HouseTake, TMap<FString, FActiveBet>& BetMap)
{
	for (auto It = BetMap.CreateConstIterator(); It; ++It)
	{
		if (It.Value().winner == Winner)
		{
			InMemoryProfiles[It.Key()].credits += It.Value().amount * 2;
			MoneyWon += It.Value().amount;
		}
		else
		{
			HouseTake += It.Value().amount;
		}
	}
	BetMap.Empty();
}

void FTwitchHype::ParseABet(const TArray<FString>& ParsedCommand, FUserProfile* Profile, const FString& Username, TMap<FString, FActiveBet>& BetMap)
{
	if (!bBettingOpen)
	{
		FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s betting is not open right now!"), *ChannelName, *Username);
		client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
	}
	else if (ParsedCommand.Num() < 3)
	{
		FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s you must bet in the format \"%s <winner> <amount>\" !"), *ParsedCommand[0], *ChannelName, *Username);
		client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
	}
	else if (BetMap.Find(Username))
	{
		FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s you've already placed a bet!"), *ChannelName, *Username);
		client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
	}
	else
	{
		FActiveBet NewBet;
		NewBet.winner = ParsedCommand[1];
		NewBet.amount = FCString::Atoi(*ParsedCommand[2]);

		// Need support for red and blue bets for teams, only works for duels and DM now

		int32 ActivePlayerIndex = ActivePlayers.Find(NewBet.winner);

		if (NewBet.amount > Profile->credits || NewBet.amount <= 0)
		{
			FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s you only have %d credits to wager!"), *ChannelName, *Username, Profile->credits);
			client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
		}
		else if (NewBet.amount > MaxBet)
		{
			FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s %d is over the max bet value of %d!"), *ChannelName, *Username, NewBet.amount, MaxBet);
			client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
		}
		else if (ActivePlayerIndex == INDEX_NONE)
		{
			FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s I'm sorry, but %s is not an active player in the match!"), *ChannelName, *Username, *NewBet.winner);
			client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
		}
		else
		{
			BetMap.Add(Username, NewBet);

			Profile->credits -= NewBet.amount;

			if (bPrintBetConfirmations)
			{
				FString PlacedBet = FString::Printf(TEXT("PRIVMSG %s :%s you've placed %d on %s using %s"), *ChannelName, *Username, NewBet.amount, *NewBet.winner, *ParsedCommand[0]);
				client.SendIRC(TCHAR_TO_ANSI(*PlacedBet));
			}
		}
	}

}

void FTwitchHype::PrintTop10()
{
	if (LastTop10Time > 0 && FPlatformTime::Seconds() - LastTop10Time < Top10CooldownTime)
	{
		return;
	}

	// Ultra laziness, let the db do the sorting
	FlushToDB();

	FString QueryCommand = TEXT("SELECT name, credits FROM Users ORDER BY credits DESC LIMIT 10");
	sqlite3_stmt *sqlStatement;
	if (sqlite3_prepare_v2(db, TCHAR_TO_ANSI(*QueryCommand), -1, &sqlStatement, NULL) == SQLITE_OK)
	{
		int32 Place = 1;
		while (sqlite3_step(sqlStatement) == SQLITE_ROW)
		{
			FString Name = FString(ANSI_TO_TCHAR((const char*)sqlite3_column_text(sqlStatement, 0)));
			int32 Credits = sqlite3_column_int(sqlStatement, 1);
			
			FString Top10Text = FString::Printf(TEXT("PRIVMSG %s :%d. %s - %d"), *ChannelName, Place, *Name, Credits);
			client.SendIRC(TCHAR_TO_ANSI(*Top10Text));
			Place++;
		}
	}
	sqlite3_finalize(sqlStatement);
	LastTop10Time = FPlatformTime::Seconds();
}

void FTwitchHype::GiveExtraMoney(FUserProfile* Profile, const FString& Username)
{
	if (Profile->credits < InitialCredits && !HasActiveBets(Username))
	{
		Profile->credits = InitialCredits;
		Profile->bankrupts++;

		FString Bankrupt = FString::Printf(TEXT("PRIVMSG %s :%s you've been restored to %d credits, you've gone bankrupt %d times"), *ChannelName, *Username, InitialCredits, Profile->bankrupts);
		client.SendIRC(TCHAR_TO_ANSI(*Bankrupt));
	}
}

bool FTwitchHype::HasActiveBets(const FString& Username)
{
	if (ActiveBets.Find(Username))
	{
		return true;
	}

	if (ActiveFirstBloodBets.Find(Username))
	{
		return true;
	}

	if (ActiveFirstSuicideBets.Find(Username))
	{
		return true;
	}

	return false;
}

void FTwitchHype::UndoBets(FUserProfile* Profile, const FString& Username)
{
	FActiveBet* ActiveBet = nullptr;
	
	ActiveBet = ActiveBets.Find(Username);
	if (ActiveBet)
	{
		Profile->credits += ActiveBet->amount;
		ActiveBets.Remove(Username);
	}

	ActiveBet = ActiveFirstBloodBets.Find(Username);
	if (ActiveBet)
	{
		Profile->credits += ActiveBet->amount;
		ActiveFirstBloodBets.Remove(Username);
	}

	ActiveBet = ActiveFirstSuicideBets.Find(Username);
	if (ActiveBet)
	{
		Profile->credits += ActiveBet->amount;
		ActiveFirstSuicideBets.Remove(Username);
	}
}

void FTwitchHype::SendChat(const FString& Command, FUserProfile* Profile, const FString& Username)
{
	if (Profile->credits < ChatCost)
	{
		FString Bankrupt = FString::Printf(TEXT("PRIVMSG %s :%s, it costs %d to chat, you only have %d!"), *ChannelName, *Username, ChatCost, Profile->credits);
		client.SendIRC(TCHAR_TO_ANSI(*Bankrupt));

		return;
	}

	Profile->credits -= ChatCost;

	FString ChatText = Command;
	ChatText.RemoveFromStart(TEXT("!chat "));
	FString Message = Username + TEXT(" says: ") + ChatText;
	
	for (auto World : KnownWorlds)
	{
		for (FConstPlayerControllerIterator Iterator = World->GetPlayerControllerIterator(); Iterator; ++Iterator)
		{
			AUTBasePlayerController* UTPC = Cast<AUTPlayerController>(*Iterator);
			if (UTPC != nullptr)
			{
				UTPC->ClientSay(nullptr, Message, ChatDestinations::Local);
			}
		}
	}
}