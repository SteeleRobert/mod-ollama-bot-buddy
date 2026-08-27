#pragma once

// Gameobject guids shown to the model are offset into their own numeric range so
// they can never collide with creature spawn ids (see mod-ollama-bot-buddy_api.cpp).
#ifndef GO_GUID_OFFSET
#define GO_GUID_OFFSET 1000000000u
#endif
#include "Player.h"
#include <string>
#include <vector>

enum class BotControlCommandType
{
    MoveTo,
    MoveToTarget,
    Attack,
    Interact,
    CastSpell,
    Loot,
    Follow,
    Say,
    AcceptQuest,
    TurnInQuest,
    Stop
};

struct BotControlCommand
{
    BotControlCommandType type;
    std::vector<std::string> args;
};

bool HandleBotControlCommand(Player* bot, const BotControlCommand& command);
bool ParseBotControlCommand(Player* bot, const std::string& commandStr);

std::string FormatCommandString(const BotControlCommand& command);


// BotBuddyAI namespace with wrappers for bot actions
namespace BotBuddyAI
{
    bool MoveTo(Player* bot, float x, float y, float z);
    bool Attack(Player* bot, ObjectGuid guid);
    bool CastSpell(Player* bot, uint32 spellId, Unit* target = nullptr);
    bool Say(Player* bot, const std::string& msg);
    bool FollowMaster(Player* bot);
    bool StopMoving(Player* bot);
    /// Quest handling functions
    bool AcceptQuest(Player* bot, uint32 questId);
    bool TurnInQuest(Player* bot, uint32 questId);
    bool InteractWithQuestGiver(Player* bot, WorldObject* questGiver);
    /// "Talin Keeneye (guid: N, Position: x y z, Distance: d)" for the NPC that
    /// takes questId when finished; empty if none is known.
    std::string QuestEnderHint(Player* bot, uint32 questId);
    /// Where a quest item comes from on this server - "drops from X (guid, position,
    /// distance)" / "found inside Y ..." - resolved from loot tables, cached per item.
    std::string QuestItemSourceHint(Player* bot, uint32 itemId);
    /// Nearest live spawn of a kill-objective creature, as a place the model can use.
    std::string QuestKillTargetHint(Player* bot, uint32 creatureEntry);
    bool AutoNavigateGossipForQuests(Player* bot, Creature* creature);
    bool HasQuestsAvailable(Player* bot, WorldObject* questGiver);
    /// Loot a corpse. lowGuid is the guid the model saw in its visible list;
    /// 0 means "the nearest corpse you are allowed to loot".
    bool LootCorpse(Player* bot, uint32 lowGuid);
    bool Interact(Player* bot, ObjectGuid guid);
    
    // Quest-related helper functions
    bool InteractWithQuestGiver(Player* bot, WorldObject* questGiver);
    bool AutoNavigateGossipForQuests(Player* bot, Creature* creature);
    bool HasQuestsAvailable(Player* bot, WorldObject* questGiver);
    std::vector<Creature*> GetNearbyQuestGivers(Player* bot, float radius = 50.0f);
}
