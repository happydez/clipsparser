#include <sourcemod>
#include <sdktools>
#include <cstrike>

#pragma semicolon 1
#pragma newdecls required

ConVar sv_cheats;

public void OnPluginStart()
{
	sv_cheats = FindConVar("sv_cheats");
}

public void OnClientPutInServer(int client)
{
	if (IsValidClient(client))
	{
		sv_cheats.ReplicateToClient(client, "1");
	}
}

bool IsValidClient(int client)
{
	return client > 0 && client <= MaxClients && IsClientInGame(client) && !IsFakeClient(client);
}
