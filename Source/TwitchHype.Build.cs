// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

using System.IO;

namespace UnrealBuildTool.Rules
{
	public class TwitchHype : ModuleRules
	{
        public TwitchHype(TargetInfo Target)
        {
			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
                    "Engine",
                    "UnrealTournament",
					"InputCore",
					"SlateCore",
					"RenderCore",
					"RHI"
				}
				);

            var LIBPath = Path.Combine("..", "..", "UnrealTournament", "Plugins", "TwitchHype", "Source", "lib");

            var SQLLibPath = Path.Combine(LIBPath, "sqlite3.lib");
            
			// Lib file
            PublicLibraryPaths.Add(LIBPath);
            PublicAdditionalLibraries.Add(SQLLibPath);
            //Definitions.Add("BGDWIN32");
            //Definitions.Add("NONDLL");
		}
	}
}