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
	PrintBetConfirmations = 0;
	TopTenCooldownTime = 60;
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
	LastTop10Time = 0;
	InitialCredits = 1500;

	ATwitchHype* Settings = ATwitchHype::StaticClass()->GetDefaultObject<ATwitchHype>();
	// Load these from config file
	ChannelName = Settings->ChannelName;
	BotNickname = Settings->BotNickname;
	OAuth = Settings->OAuth;
	bPrintBetConfirmations = (Settings->PrintBetConfirmations > 0);
	Top10CooldownTime = Settings->TopTenCooldownTime;

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
		if (client.Connected())
		{
			return true;
		}

		bAuthenticated = false;
		bJoinedChannel = false;
		client.Debug(true);
		if (client.InitSocket())
		{
			FString host = TEXT("irc.twitch.tv");
			int32 port = 6667;
			// non-blocking connect
			if (!client.Connect(TCHAR_TO_ANSI(*host), port))
			{
			}
		}

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
			char *zSQL = sqlite3_mprintf("INSERT INTO Users (name, credits, bankrupts) VALUES(%Q, %d)", message.prefix.nick.c_str(), Profile.credits, 0);
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
				ParseBet(ParsedCommand, Profile, Username);
			}
			else if (ParsedCommand[0] == TEXT("!firstbloodbet"))
			{
				ParseFirstBloodBet(ParsedCommand, Profile, Username);
			}
			else if (ParsedCommand[0] == TEXT("!top10"))
			{
				PrintTop10();
			}
			else if (ParsedCommand[0] == TEXT("!ineedmoney"))
			{
				GiveExtraMoney(Profile, Username);
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
		FString WaitingPostMatch = FString::Printf(TEXT("PRIVMSG %s :The match is over, thanks for betting!"), *ChannelName);
		client.SendIRC(TCHAR_TO_ANSI(*WaitingPostMatch));
		
		if (GM && GM->UTGameState && GM->UTGameState->WinnerPlayerState)
		{
			int32 MoneyWon = 0;
			int32 HouseTake = 0;
			AwardBets(GM->UTGameState->WinnerPlayerState->PlayerName, MoneyWon, HouseTake);
			
			FString BettingStats = FString::Printf(TEXT("PRIVMSG %s :Betting stats: %d credits paid out, %d credits lost"), *ChannelName, MoneyWon, HouseTake);
			client.SendIRC(TCHAR_TO_ANSI(*BettingStats));
		}
		else
		{
			ForgiveBets();
		}

		ActivePlayers.Empty();
	}
	else if (NewState == MatchState::InProgress)
	{
		FString InProgress = FString::Printf(TEXT("PRIVMSG %s :The match is starting, betting is now closed!"), *ChannelName);
		client.SendIRC(TCHAR_TO_ANSI(*InProgress));

		bFirstBlood = false;
		bBettingOpen = false;
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
			FString FirstBlood = FString::Printf(TEXT("PRIVMSG %s :First Blood goes to %s!"), *ChannelName, *Killer->PlayerState->PlayerName);
			client.SendIRC(TCHAR_TO_ANSI(*FirstBlood));

			int32 MoneyWon = 0;
			int32 HouseTake = 0;
			AwardFirstBloodBets(Killer->PlayerState->PlayerName, MoneyWon, HouseTake);

			FString BettingStats = FString::Printf(TEXT("PRIVMSG %s :Betting stats: %d credits paid out, %d credits lost"), *ChannelName, MoneyWon, HouseTake);
			client.SendIRC(TCHAR_TO_ANSI(*BettingStats));
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
}

void FTwitchHype::AwardBets(const FString& Winner, int32& MoneyWon, int32& HouseTake)
{
	for (auto It = ActiveBets.CreateConstIterator(); It; ++It)
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
	ActiveBets.Empty();
}

void FTwitchHype::AwardFirstBloodBets(const FString& Winner, int32& MoneyWon, int32& HouseTake)
{
	for (auto It = ActiveFirstBloodBets.CreateConstIterator(); It; ++It)
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
	ActiveFirstBloodBets.Empty();
}

void FTwitchHype::ParseBet(const TArray<FString>& ParsedCommand, FUserProfile* Profile, const FString& Username)
{
	if (!bBettingOpen)
	{
		FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s betting is not open right now!"), *ChannelName, *Username);
		client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
	}
	else if (ParsedCommand.Num() < 3)
	{
		FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s you must bet in the format \"!bet <winner> <amount>\" !"), *ChannelName, *Username);
		client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
	}
	else if (ActiveBets.Find(Username))
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
		else if (ActivePlayerIndex == INDEX_NONE)
		{
			FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s I'm sorry, but %s is not an active player in the match!"), *ChannelName, *Username, *NewBet.winner);
			client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
		}
		else
		{
			ActiveBets.Add(Username, NewBet);

			Profile->credits -= NewBet.amount;

			if (bPrintBetConfirmations)
			{
				FString PlacedBet = FString::Printf(TEXT("PRIVMSG %s :%s you've placed %d on %s"), *ChannelName, *Username, NewBet.amount, *NewBet.winner);
				client.SendIRC(TCHAR_TO_ANSI(*PlacedBet));
			}
		}
	}
}

void FTwitchHype::ParseFirstBloodBet(const TArray<FString>& ParsedCommand, FUserProfile* Profile, const FString& Username)
{
	if (!bBettingOpen)
	{
		FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s betting is not open right now!"), *ChannelName, *Username);
		client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
	}
	else if (ParsedCommand.Num() < 3)
	{
		FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s you must bet in the format \"!firstbloodbet <winner> <amount>\" !"), *ChannelName, *Username);
		client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
	}
	else if (ActiveFirstBloodBets.Find(Username))
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
		else if (ActivePlayerIndex == INDEX_NONE)
		{
			FString InvalidBet = FString::Printf(TEXT("PRIVMSG %s :%s I'm sorry, but %s is not an active player in the match!"), *ChannelName, *Username, *NewBet.winner);
			client.SendIRC(TCHAR_TO_ANSI(*InvalidBet));
		}
		else
		{
			ActiveFirstBloodBets.Add(Username, NewBet);

			Profile->credits -= NewBet.amount;

			if (bPrintBetConfirmations)
			{
				FString PlacedBet = FString::Printf(TEXT("PRIVMSG %s :%s you've placed %d on %s to take first blood"), *ChannelName, *Username, NewBet.amount, *NewBet.winner);
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
	if (Profile->credits < InitialCredits)
	{
		Profile->credits = InitialCredits;
		Profile->bankrupts++;

		FString Bankrupt = FString::Printf(TEXT("PRIVMSG %s :%s you've been restored to %d credits, you've gone bankrupt %d times"), *ChannelName, *Username, InitialCredits, Profile->bankrupts);
		client.SendIRC(TCHAR_TO_ANSI(*Bankrupt));
	}
}