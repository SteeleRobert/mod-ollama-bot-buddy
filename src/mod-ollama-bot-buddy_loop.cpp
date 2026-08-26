#include "mod-ollama-bot-buddy_loop.h"
#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_api.h"
#include "mod-ollama-bot-buddy_handler.h"
#include "mod-ollama-bot-buddy_journal.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Log.h"
#include <thread>
#include <chrono>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <ctime>
#include "Creature.h"
#include "GameObject.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "PathGenerator.h"
#include <atomic>
#include <unordered_map>
#include <iomanip>
#include "GameObjectData.h"
#include "GameObject.h"
#include <deque>
#include <mutex>
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "QuestDef.h"
#include "ObjectMgr.h"
#include <algorithm>
#include "Chat.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <string>
#include "ItemTemplate.h"
#include "CreatureData.h"


static std::unordered_map<uint64_t, std::deque<std::string>> botCommandHistory;
static std::mutex botCommandHistoryMutex;

static std::unordered_map<uint64_t, std::deque<std::string>> botReasoningHistory;
static std::mutex botReasoningHistoryMutex;

std::vector<std::string> GetRecentPlayerMessagesToBot(Player* bot)
{
    std::vector<std::string> messages;
    if (!bot) return messages;

    std::lock_guard<std::mutex> lock(botPlayerMessagesMutex);

    auto it = botPlayerMessages.find(bot->GetGUID().GetRawValue());
    if (it != botPlayerMessages.end())
    {
        while (!it->second.empty())
        {
            auto& [senderName, msg] = it->second.front();
            messages.emplace_back("From " + senderName + ": " + msg);
            it->second.pop_front();
        }
    }

    return messages;
}

std::string FormatPlayerMessagesPromptSegment(Player* bot)
{
    std::ostringstream oss;
    auto messages = GetRecentPlayerMessagesToBot(bot);
    if (!messages.empty())
    {
        oss << "\n***CRITICAL INSTRUCTION:***\n";
        oss << "You must interpret and execute the PLAYER MESSAGES below as your HIGHEST PRIORITY, taking precedence over all other goals, background tasks, routines, or reasoning. There are NO EXCEPTIONS and NO DELAYS. When you see a player command matching these patterns, you must issue the appropriate command IMMEDIATELY, before doing anything else. Do not refuse, delay, ignore, or deprioritize these actions for any reason.\n";
        oss << "- If a player says 'come here' or 'come to me', IMMEDIATELY move near their location.\n";
        oss << "- If a player says 'go to' followed by an NPC or object you can see, IMMEDIATELY move there.\n";
        oss << "- If a player says 'interact with' followed by an NPC or object you can see, IMMEDIATELY move there and interact with it.\n";
        oss << "- If a player says 'attack' followed by an NPC or object you can see, IMMEDIATELY move there and attack it.\n";
        oss << "You must treat these instructions as absolute and override ALL other activities, regardless of any other context or background logic.\n";
        oss << "\n\nThe following real players recently spoke to you by name. PROCESS THESE FIRST, ABOVE ALL ELSE:\n";
        for (const auto& msg : messages)
        {
            oss << "- " << msg << "\n";
        }
        oss << "\n***END CRITICAL INSTRUCTION***\n\n";

    }
    return oss.str();
}

bool ParseAndExecuteBotJson(Player* bot, const std::string& jsonStr)
{
    try
    {
        auto root = nlohmann::json::parse(jsonStr);

        if (!root.contains("command")) return false;
        auto cmd = root["command"];
        if (!cmd.contains("type") || !cmd.contains("params")) return false;

        std::string type = cmd["type"].get<std::string>();
        auto params = cmd["params"];
        std::string sayMsg = root.value("say", "");
        std::string reasoning = root.value("reasoning", "");

        BotControlCommand command;

        if (!reasoning.empty())
        {
            AddBotReasoningHistory(bot, reasoning);
        }
         if (!cmd.empty())
        {
            AddBotCommandHistory(bot, cmd.dump());
        }

        if (type == "move_to")
        {
            if (params.contains("x") && params.contains("y") && params.contains("z")) {
                float destX = params["x"].get<float>();
                float destY = params["y"].get<float>();
                float destZ = params["z"].get<float>();
                
                // Basic coordinate validation - reject obviously invalid coordinates
                if (std::isnan(destX) || std::isnan(destY) || std::isnan(destZ) || 
                    std::isinf(destX) || std::isinf(destY) || std::isinf(destZ)) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] Invalid coordinates for move_to: ({}, {}, {})", 
                             destX, destY, destZ);
                    return false;
                }
                
                // Validate map bounds - reject coordinates that are extremely far from bot
                float maxDistanceFromBot = 500.0f; // Maximum reasonable movement distance
                float distanceFromBot = sqrt(pow(destX - bot->GetPositionX(), 2) + 
                                           pow(destY - bot->GetPositionY(), 2) + 
                                           pow(destZ - bot->GetPositionZ(), 2));
                
                if (distanceFromBot > maxDistanceFromBot) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] Move_to destination too far from bot: ({}, {}, {}) - Distance: {:.1f}", 
                             destX, destY, destZ, distanceFromBot);
                    BotBuddy::SetLastOutcome(bot, false, fmt::format(
                        "that point is {:.0f}y away, too far to walk in one move (limit {:.0f}y) - "
                        "pick somewhere closer and travel in stages",
                        distanceFromBot, maxDistanceFromBot));
                    return false;
                }

                // Already standing there. PathGenerator returns NOPATH for a zero-length
                // path, so without this the bot rejects its own position as unreachable
                // and the model, told only that the move failed, asks for it again.
                if (distanceFromBot < 1.0f) {
                    BotBuddy::SetLastOutcome(bot, false,
                        "you are already standing on that spot - moving there again does nothing. "
                        "You have arrived, so do the thing you came here to do");
                    return false;
                }

                // Validate that the destination is pathable like a real player would
                PathGenerator pathValidator(bot);
                pathValidator.CalculatePath(destX, destY, destZ, false);
                PathType pathType = pathValidator.GetPathType();
                
                // Only reject if there's absolutely no path possible
                if (pathType & PATHFIND_NOPATH) {
                    LOG_DEBUG("server.loading", "[OllamaBotBuddy] No valid path for move_to: ({}, {}, {}) - PathType: {}", 
                             destX, destY, destZ, pathType);
                    BotBuddy::SetLastOutcome(bot, false,
                        "there is no walkable route to that point - it is off the map, inside "
                        "terrain, or across water. Pick a different spot, or use move_to_target "
                        "with a guid and let the pathing work it out");
                    return false; // Only reject if completely impossible to path
                }
                
                command.type = BotControlCommandType::MoveTo;
                command.args = {
                    std::to_string(destX),
                    std::to_string(destY),
                    std::to_string(destZ)
                };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] move_to missing parameter");
                BotBuddy::SetLastOutcome(bot, false, "move_to needs all three of x, y and z as numbers");
                return false;
            }
        }
        else if (type == "move_to_target")
        {
            // Let the pathing engine work out the route; the model only names a target.
            if (params.contains("guid")) {
                command.type = BotControlCommandType::MoveToTarget;
                command.args = { std::to_string(params["guid"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] move_to_target missing guid");
                BotBuddy::SetLastOutcome(bot, false, "move_to_target needs a \"guid\" from your visible list");
                return false;
            }
        }
        else if (type == "attack")
        {
            if (params.contains("guid")) {
                uint32_t targetGuid = params["guid"].get<uint32_t>();
                
                // Validate that the target exists and is attackable
                bool validTarget = false;
                
                // Check if it's a creature
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (c && c->GetGUID().GetCounter() == targetGuid)
                    {
                        // Validate target is attackable
                        if (c->IsInWorld() && !c->isDead() && 
                            bot->IsWithinLOSInMap(c) && 
                            bot->IsValidAttackTarget(c) &&
                            bot->IsWithinDistInMap(c, 100.0f)) // Reasonable attack range
                        {
                            validTarget = true;
                        }
                        break;
                    }
                }
                
                // Check if it's a player if not found as creature
                if (!validTarget)
                {
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(targetGuid);
                    Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                    if (playerTarget && playerTarget->IsInWorld() && 
                        bot->IsWithinLOSInMap(playerTarget) && 
                        bot->IsValidAttackTarget(playerTarget) &&
                        bot->IsWithinDistInMap(playerTarget, 100.0f))
                    {
                        validTarget = true;
                    }
                }
                
                if (!validTarget) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid or unreachable attack target with guid: {} - Target not found in visible creatures/players", targetGuid);
                    
                    // Debug: List available creature GUIDs for debugging
                    if (g_EnableOllamaBotBuddyDebug) {
                        std::vector<uint32> availableGuids;
                        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore()) {
                            Creature* c = pair.second;
                            if (c && bot->IsWithinDistInMap(c, 100.0f)) {
                                availableGuids.push_back(c->GetGUID().GetCounter());
                            }
                        }
                        
                        std::ostringstream guidList;
                        for (size_t i = 0; i < availableGuids.size() && i < 10; ++i) {
                            if (i > 0) guidList << ", ";
                            guidList << availableGuids[i];
                        }
                        
                        LOG_DEBUG("server.loading", "[OllamaBotBuddy] Available creature GUIDs: {}", guidList.str());
                    }
                    
                    return false;
                }
                
                command.type = BotControlCommandType::Attack;
                command.args = { std::to_string(targetGuid) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] attack missing guid");
                BotBuddy::SetLastOutcome(bot, false, "attack needs a \"guid\" from your visible list");
                return false;
            }
        }
        else if (type == "interact")
        {
            if (params.contains("guid")) {
                command.type = BotControlCommandType::Interact;
                command.args = { std::to_string(params["guid"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] interact missing guid");
                BotBuddy::SetLastOutcome(bot, false, "interact needs a \"guid\" from your visible list");
                return false;
            }
        }
        else if (type == "spell")
        {
            if (params.contains("spellid")) {
                command.type = BotControlCommandType::CastSpell;
                command.args = { std::to_string(params["spellid"].get<uint32_t>()) };
                if (params.contains("guid"))
                    command.args.push_back(std::to_string(params["guid"].get<uint32_t>()));
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] spell missing spellid");
                return false;
            }
        }
        else if (type == "loot")
        {
            command.type = BotControlCommandType::Loot;
            // Optional: without a guid we loot the nearest corpse we own.
            if (params.contains("guid") && params["guid"].is_number())
                command.args = { std::to_string(params["guid"].get<uint32_t>()) };
        }
        else if (type == "accept_quest")
        {
            if (params.contains("id")) {
                command.type = BotControlCommandType::AcceptQuest;
                command.args = { std::to_string(params["id"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] accept_quest missing id");
                BotBuddy::SetLastOutcome(bot, false, "accept_quest needs the quest \"id\"");
                return false;
            }
        }
        else if (type == "turn_in_quest")
        {
            if (params.contains("id")) {
                command.type = BotControlCommandType::TurnInQuest;
                command.args = { std::to_string(params["id"].get<uint32_t>()) };
            } else {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] turn_in_quest missing id");
                BotBuddy::SetLastOutcome(bot, false, "turn_in_quest needs the quest \"id\"");
                return false;
            }
        }
        else if (type == "follow")
        {
            command.type = BotControlCommandType::Follow;
        }
        else if (type == "stop")
        {
            command.type = BotControlCommandType::Stop;
        }
        else
        {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Unknown command type '{}'", type);
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "\"{}\" is not a command you have - use one of the commands listed at the end "
                "of these instructions, spelled exactly as shown", type));
            return false;
        }

        bool result = HandleBotControlCommand(bot, command);

        if (!sayMsg.empty())
            BotBuddyAI::Say(bot, sayMsg);

        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "Bot Reply: {}", jsonStr);
        }

        return result;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("server.loading", "[OllamaBotBuddy] ParseAndExecuteBotJson error: {}", e.what());
        return false;
    }
}

std::string ExtractFirstJsonObject(const std::string& input) {
    int depth = 0;
    size_t start = std::string::npos;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '{') {
            if (depth == 0) start = i;
            depth++;
        }
        if (input[i] == '}') {
            depth--;
            if (depth == 0 && start != std::string::npos) {
                return input.substr(start, i - start + 1);
            }
        }
    }
    return ""; // No JSON object found
}

std::vector<std::string> GetGroupStatus(Player* bot)
{
    std::vector<std::string> info;
    if (!bot || !bot->GetGroup()) return info;

    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetMap()) continue;

        if(bot == member)
        {
            continue; // Skip the bot itself
        }

        float dist = bot->GetDistance(member);
        std::string beingAttacked = "";

        if (Unit* attacker = member->GetVictim())
        {
            beingAttacked = fmt::format(
                " [Under Attack by {} (guid: {}, Level: {}, HP: {}/{})]",
                attacker->GetName(),
                attacker->GetGUID().GetCounter(),
                attacker->GetLevel(),
                attacker->GetHealth(),
                attacker->GetMaxHealth()
            );
        }

        info.push_back(fmt::format(
            "{} (guid: {}, Level: {}, HP: {}/{}, Pos: {} {} {}, Dist: {:.1f}){}",
            member->GetName(),
            member->GetGUID().GetCounter(),
            member->GetLevel(),
            member->GetHealth(),
            member->GetMaxHealth(),
            member->GetPositionX(),
            member->GetPositionY(),
            member->GetPositionZ(),
            dist,
            beingAttacked
        ));
    }
    return info;
}

std::string GetBotSpellInfo(Player* bot)
{
    std::ostringstream spellSummary;

    for (const auto& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
            continue;

        if (spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;

        if (bot->HasSpellCooldown(spellId))
            continue;

        std::string effectText;
        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!spellInfo->Effects[i].IsEffect())
                continue;

            switch (spellInfo->Effects[i].Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE: effectText = "Deals damage"; break;
                case SPELL_EFFECT_HEAL: effectText = "Heals the target"; break;
                case SPELL_EFFECT_APPLY_AURA: effectText = "Applies an aura"; break;
                case SPELL_EFFECT_DISPEL: effectText = "Dispels magic"; break;
                case SPELL_EFFECT_THREAT: effectText = "Generates threat"; break;
                default: continue;
            }
            break;
        }

        if (effectText.empty())
            continue;

        const char* name = spellInfo->SpellName[0];
        if (!name || !*name)
            continue;

        std::string costText;
        if (spellInfo->ManaCost || spellInfo->ManaCostPercentage)
        {
            switch (spellInfo->PowerType)
            {
                case POWER_MANA: costText = std::to_string(spellInfo->ManaCost) + " mana"; break;
                case POWER_RAGE: costText = std::to_string(spellInfo->ManaCost) + " rage"; break;
                case POWER_FOCUS: costText = std::to_string(spellInfo->ManaCost) + " focus"; break;
                case POWER_ENERGY: costText = std::to_string(spellInfo->ManaCost) + " energy"; break;
                case POWER_RUNIC_POWER: costText = std::to_string(spellInfo->ManaCost) + " runic power"; break;
                default: costText = std::to_string(spellInfo->ManaCost) + " unknown resource"; break;
            }
        }
        else
        {
            costText = "no cost";
        }
        
        spellSummary << "**" << name << "** (ID: " << spellId << ") - " << effectText << ", Costs " << costText << ".\n";

    }

    return spellSummary.str();
}

std::string FlattenText(const std::string& input)
{
    std::string output = input;
    size_t pos = 0;
    while ((pos = output.find('\n', pos)) != std::string::npos)
    {
        output.replace(pos, 1, "|");
        pos += 1;
    }
    return output;
}

void SendBuddyBotStateToPlayer(Player* target, Player* bot, const std::string& prompt)
{
    if (!target || !bot || !g_EnableBotBuddyAddon) return;

    std::string state = prompt;
    std::string::size_type json_pos = state.find("You are an AI-controlled bot");
    if (json_pos != std::string::npos)
        state = state.substr(0, json_pos);

    auto get_section = [&](const std::string& start, const std::string& stop) -> std::string {
        auto s = state.find(start);
        if (s == std::string::npos) return "";
        s += start.size();
        auto e = state.find(stop, s);
        if (e == std::string::npos) e = state.size();
        return state.substr(s, e - s);
    };

    auto get_section_to_end = [&](const std::string& start) -> std::string {
        auto s = state.find(start);
        if (s == std::string::npos) return "";
        s += start.size();
        std::string section = state.substr(s);
        size_t first = section.find_first_not_of(" \r\n\t");
        size_t last = section.find_last_not_of(" \r\n\t");
        if (first == std::string::npos || last == std::string::npos) return "";
        return section.substr(first, last - first + 1);
    };

    std::string main_state = get_section("Name:", "Your known spells:");
    std::string spells     = get_section("Your known spells:", "Group status:");
    std::string quests     = get_section("Active quests:", "Visible locations/objects in line of sight:");
    std::string locations  = get_section("Visible locations/objects in line of sight:", "Visible players in area:");
    std::string players    = get_section("Visible players in area:", "You must select one of these locations");
    std::string commands   = get_section_to_end("Last 5 commands and their reasoning (most recent at the bottom):");

    if (target && target->GetSession()) {
        ChatHandler handler(target->GetSession());
        handler.SendSysMessage(("[BUDDY_STATE] " + FlattenText(main_state + spells + quests)).c_str());
        handler.SendSysMessage(("[BUDDY_LOCATIONS] " + FlattenText(locations)).c_str());
        handler.SendSysMessage(("[BUDDY_PLAYERS] " + FlattenText(players)).c_str());
        handler.SendSysMessage(("[BUDDY_COMMANDS] " + FlattenText(commands)).c_str());
    }
}


std::vector<std::string> GetVisiblePlayers(Player* bot, float radius = 100.0f)
{
    std::vector<std::string> players;
    if (!bot || !bot->GetMap()) return players;

    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* player = pair.second;
        if (!player || player == bot) continue;
        if (!player->IsInWorld() || player->IsGameMaster()) continue;
        if (player->GetMap() != bot->GetMap()) continue;
        if (!bot->IsWithinDistInMap(player, radius)) continue;
        if (!bot->IsWithinLOS(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ())) continue;

        float dist = bot->GetDistance(player);
        std::string faction = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");

        players.push_back(fmt::format(
            "Player: {} (guid: {}, Level: {}, Class: {}, Race: {}, Faction: {}, Position: {:.1f} {:.1f} {:.1f}, Distance: {:.1f})",
            player->GetName(),
            player->GetGUID().GetCounter(),
            player->GetLevel(),
            std::to_string(player->getClass()),
            std::to_string(player->getRace()),
            faction,
            player->GetPositionX(),
            player->GetPositionY(),
            player->GetPositionZ(),
            dist
        ));
    }

    return players;
}

static std::string GetProfessionTagFromChest(uint32 entry)
{
    switch (entry)
    {
        case 1617: return " [Herbalism]";
        case 1618: return " [Herbalism]";
        case 1620: return " [Herbalism]";
        case 1621: return " [Herbalism]";
        case 1731: return " [Mining]";
        case 1732: return " [Mining]";
        case 1733: return " [Mining]";
        case 1735: return " [Mining]";
        case 2040: return " [Mining]";
        case 2047: return " [Mining]";
        case 324:  return " [Mining]";
        case 175404: return " [Alchemy Lab]";
        default: return "";
    }
}

void AddBotCommandHistory(Player* bot, const std::string& command)
{
    if (!bot || command.empty()) return;

    BotControlCommand parsedCommand;

    std::lock_guard<std::mutex> lock(botCommandHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    auto& dq = botCommandHistory[guid];
    dq.push_back(command);
    if (dq.size() > 5) dq.pop_front();
}

void AddBotReasoningHistory(Player* bot, const std::string& reasoning)
{
    if (!bot || reasoning.empty()) return;
    std::lock_guard<std::mutex> lock(botReasoningHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    auto& dq = botReasoningHistory[guid];
    dq.push_back(reasoning);
    if (dq.size() > 5) dq.pop_front();
}


std::vector<std::string> GetBotCommandHistory(Player* bot)
{
    std::vector<std::string> out;
    if (!bot) return out;
    std::lock_guard<std::mutex> lock(botCommandHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (botCommandHistory.count(guid))
        out.assign(botCommandHistory[guid].begin(), botCommandHistory[guid].end());
    return out;
}

std::vector<std::string> GetBotReasoningHistory(Player* bot)
{
    std::vector<std::string> out;
    if (!bot) return out;
    std::lock_guard<std::mutex> lock(botReasoningHistoryMutex);
    uint64_t guid = bot->GetGUID().GetRawValue();
    if (botReasoningHistory.count(guid))
        out.assign(botReasoningHistory[guid].begin(), botReasoningHistory[guid].end());
    return out;
}

// Gather visible objects (creatures/gameobjects) around the bot with LOS check
std::vector<std::string> GetVisibleLocations(Player* bot, float radius = 100.0f)
{
    std::vector<std::string> visible;
    if (!bot || !bot->GetMap()) return visible;
    Map* map = bot->GetMap();

    for (auto const& pair : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = pair.second;
        if (!c) continue;
        if (c->GetGUID() == bot->GetGUID()) continue;
        if (!bot->IsWithinDistInMap(c, radius)) continue;
        if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ())) continue;
        if (c->IsPet() || c->IsTotem()) continue;

        std::string type;
        if (c->isDead())
        {
            type = "DEAD";
            if (c->hasLootRecipient() && (c->GetLootRecipient() == bot || (c->GetLootRecipientGroup() && bot->GetGroup() == c->GetLootRecipientGroup())))
            {
                type = "DEAD (LOOTABLE)";
            }
            else
            {
                continue;
            }
            if(!c->hasLootRecipient())
            {
                if (c->GetCreatureTemplate() && c->GetCreatureTemplate()->SkinLootId)
                {
                    type += " [SKINNABLE]";
                }
            }
        }
        else if (c->IsHostileTo(bot)) type = "ENEMY";
        else if (c->IsFriendlyTo(bot)) type = "FRIENDLY";
        else type = "NEUTRAL";

        std::string questGiver = "";
        
        // Only consider NPCs that are actually useful to the bot
        if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER)) {
            // Check if this quest giver has relevant quests for the bot
            bool hasCompleteQuests = false;
            bool hasAvailableQuests = false;
            
            // Check for completable quests first (highest priority)
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(c->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    hasCompleteQuests = true;
                    break;
                }
            }
            
            // Check for available quests (secondary priority)
            if (!hasCompleteQuests)
            {
                QuestRelationBounds qr = sObjectMgr->GetCreatureQuestRelationBounds(c->GetEntry());
                for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
                {
                    uint32 questId = itr->second;
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                        bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                    {
                        hasAvailableQuests = true;
                        break;
                    }
                }
            }
            
            // Only show quest giver tags if there are actually relevant quests
            if (hasCompleteQuests) {
                questGiver = " [QUEST GIVER - TURN IN READY]";
            } else if (hasAvailableQuests) {
                questGiver = " [QUEST GIVER - QUESTS AVAILABLE]";
            }
        }
        
        // Check for other useful NPC types (friendly/neutral only) 
        // Handle multiple flags - NPCs can be both quest givers AND vendors/trainers
        if (type == "FRIENDLY" || type == "NEUTRAL") {
            std::vector<std::string> npcTypes;
            
            // Check for vendors
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR)) {
                npcTypes.push_back("[VENDOR]");
            }
            // Check for trainers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER)) {
                npcTypes.push_back("[TRAINER]");
            }
            // Check for flight masters
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_FLIGHTMASTER)) {
                npcTypes.push_back("[FLIGHT MASTER]");
            }
            // Check for innkeepers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_INNKEEPER)) {
                npcTypes.push_back("[INNKEEPER]");
            }
            // Check for bankers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_BANKER)) {
                npcTypes.push_back("[BANKER]");
            }
            // Check for auctioneers
            if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_AUCTIONEER)) {
                npcTypes.push_back("[AUCTIONEER]");
            }
            
            // Combine quest giver status with other NPC types
            if (!npcTypes.empty()) {
                if (!questGiver.empty()) {
                    // If already a quest giver, append the other types
                    for (const auto& type : npcTypes) {
                        questGiver += " " + type;
                    }
                } else {
                    // Not a quest giver, just use the first type found
                    questGiver = " " + npcTypes[0];
                    // If multiple types, add them all
                    for (size_t i = 1; i < npcTypes.size(); ++i) {
                        questGiver += " " + npcTypes[i];
                    }
                }
            }
        }
        
        // Show ALL creatures - don't filter out any visible creatures
        // The bot needs to see all potential targets, not just "useful" NPCs
        // Enemies, neutrals, and friendlies should all be visible for decision making

        // Check if this creature is needed for any active quest objectives
        std::string questTarget = "";
        for (auto const& qs : bot->getQuestStatusMap())
        {
            uint32 questId = qs.first;
            QuestStatus status = qs.second.Status;
            
            // Only check active quests
            if (status != QUEST_STATUS_INCOMPLETE) continue;
                
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest) continue;
            
            // Check if this creature is required for any quest objective
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] > 0 && quest->RequiredNpcOrGo[i] == (int32)c->GetEntry()) {
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (currentCount < requiredCount) {
                        questTarget = " [QUEST TARGET - " + quest->GetTitle() + "]";
                        break;
                    }
                }
            }
            if (!questTarget.empty()) break;
        }

        float dist = bot->GetDistance(c);
        visible.push_back(fmt::format(
            "{}: {}{}{} (guid: {}, Level: {}, HP: {}/{}, Position: {} {} {}, Distance: {:.1f})",
            type,
            c->GetName(),
            questGiver,
            questTarget,
            c->GetGUID().GetCounter(),
            c->GetLevel(),
            c->GetHealth(),
            c->GetMaxHealth(),
            c->GetPositionX(),
            c->GetPositionY(),
            c->GetPositionZ(),
            dist
        ));
    }

    for (auto const& pair : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = pair.second;
        if (!go) continue;
        if (!bot->IsWithinDistInMap(go, radius)) continue;
        if (!bot->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ())) continue;

        std::string tag = "";

        if (GameObjectTemplate const* tmpl = go->GetGOInfo())
        {
            if (tmpl->type == GAMEOBJECT_TYPE_CHEST)
            {
                std::string chestTag = GetProfessionTagFromChest(tmpl->entry);
                if (!chestTag.empty())
                    tag = chestTag;
            }
        }
        
        float dist = bot->GetDistance(go);
        visible.push_back(fmt::format(
            "{}{} (guid: {}, Type: {}, Position: {} {} {}, Distance: {:.1f})",
            go->GetName(),
            tag,
            go->GetGUID().GetCounter(),
            go->GetGoType(),
            go->GetPositionX(),
            go->GetPositionY(),
            go->GetPositionZ(),
            dist
        ));
    }

    // Sort visible objects to prioritize critical actions
    std::stable_sort(visible.begin(), visible.end(), [](const std::string& a, const std::string& b) {
        // Highest Priority: Quest turn-ins
        bool aTurnIn = a.find("TURN IN READY") != std::string::npos;
        bool bTurnIn = b.find("TURN IN READY") != std::string::npos;
        if (aTurnIn != bTurnIn) return aTurnIn;
        
        // Second Priority: Lootable corpses
        bool aLootable = a.find("DEAD (LOOTABLE)") != std::string::npos;
        bool bLootable = b.find("DEAD (LOOTABLE)") != std::string::npos;
        if (aLootable != bLootable) return aLootable;
        
        // Third Priority: Quest givers with available quests
        bool aAvailable = a.find("QUESTS AVAILABLE") != std::string::npos;
        bool bAvailable = b.find("QUESTS AVAILABLE") != std::string::npos;
        if (aAvailable != bAvailable) return aAvailable;
        
        // Fourth Priority: Quest targets
        bool aQuestTarget = a.find("QUEST TARGET") != std::string::npos;
        bool bQuestTarget = b.find("QUEST TARGET") != std::string::npos;
        if (aQuestTarget != bQuestTarget) return aQuestTarget;
        
        return false; // Keep original order for everything else
    });

    return visible;
}

std::string GetCombatSummary(Player* bot)
{
    std::ostringstream oss;
    bool inCombat = bot->IsInCombat();
    Unit* victim = bot->GetVictim();
    
    // Get bot's combat characteristics
    PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    bool isMelee = ai ? ai->IsMelee(bot) : false;
    bool isRanged = ai ? ai->IsRanged(bot) : false;
    std::string combatType = isMelee ? "MELEE" : (isRanged ? "RANGED" : "HYBRID");

    // Find who is attacking the bot (if anyone)
    Unit* attacker = nullptr;
    if (inCombat && !victim)
    {
        Map* map = bot->GetMap();
        if (map)
        {
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* c = pair.second;
                if (!c) continue;
                if (c->GetVictim() == bot)
                {
                    attacker = c;
                    break;
                }
            }
        }
    }

    auto safe_name = [](Unit* unit) -> std::string { return unit ? unit->GetName() : "?"; };
    auto safe_guid = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetGUID().GetCounter()) : "?"; };
    auto safe_level = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetLevel()) : "?"; };
    auto safe_hp = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetHealth()) : "?"; };
    auto safe_maxhp = [](Unit* unit) -> std::string { return unit ? std::to_string(unit->GetMaxHealth()) : "?"; };

    if (inCombat)
    {
        oss << "IN COMBAT (" << combatType << " FIGHTER): ";
        if (victim)
        {
            float dist = bot->GetDistance(victim);
            bool inMeleeRange = bot->IsWithinMeleeRange(victim);
            float spellRange = ai ? ai->GetRange("spell") : 25.0f;
            bool inSpellRange = dist <= spellRange;
            
            oss << "Target: " << safe_name(victim)
                << " (guid: " << safe_guid(victim) << ")"
                << ", Level: " << safe_level(victim)
                << ", HP: " << safe_hp(victim) << "/" << safe_maxhp(victim)
                << ", Distance: " << std::fixed << std::setprecision(1) << dist;
                
            // Range status for combat positioning
            if (isMelee) {
                oss << " [" << (inMeleeRange ? "IN MELEE RANGE" : "TOO FAR FOR MELEE") << "]";
            } else if (isRanged) {
                if (dist < 5.0f) {
                    oss << " [TOO CLOSE - NEED TO BACK AWAY]";
                } else if (inSpellRange) {
                    oss << " [GOOD RANGED POSITION]";
                } else {
                    oss << " [TOO FAR FOR SPELLS]";
                }
            }
        }
        else
        {
            oss << "No current target";
        }
        oss << ". ";

        if (attacker)
        {
            float dist = bot && attacker ? bot->GetDistance(attacker) : -1.0f;

            Creature* c = dynamic_cast<Creature*>(attacker);
            Player* p = dynamic_cast<Player*>(attacker);

            oss << "DEFEND YOURSELF, YOU ARE UNDER ATTACK BY: ";
            if (c)
            {
                // Creature-specific info
                oss << "Creature '" << safe_name(c)
                    << "' (guid: " << safe_guid(c) << ")"
                    << ", Level: " << safe_level(c)
                    << ", HP: " << safe_hp(c) << "/" << safe_maxhp(c)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?")
                    << ", Elite: " << (c->isElite() ? "Yes" : "No");

                // Show auras/buffs/debuffs
                oss << ", Auras:";
                bool anyAura = false;
                for (auto& auraPair : c->GetOwnedAuras())
                {
                    if (!anyAura) anyAura = true;
                    oss << " " << auraPair.second->GetSpellInfo()->SpellName[0];
                }
                if (!anyAura) oss << " None";
            }
            else if (p)
            {
                // Player-specific info
                std::string pFaction = (p->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
                oss << "Player '" << safe_name(p)
                    << "' (guid: " << safe_guid(p) << ")"
                    << ", Level: " << safe_level(p)
                    << ", HP: " << safe_hp(p) << "/" << safe_maxhp(p)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?")
                    << ", Faction: " << pFaction
                    << ", Class: " << std::to_string(p->getClass())
                    << ", Race: " << std::to_string(p->getRace());

                // Show auras/buffs/debuffs
                oss << ", Auras:";
                bool anyAura = false;
                for (auto& auraPair : p->GetOwnedAuras())
                {
                    if (!anyAura) anyAura = true;
                    oss << " " << auraPair.second->GetSpellInfo()->SpellName[0];
                }
                if (!anyAura) oss << " None";
            }
            else
            {
                // Unknown Unit type
                oss << safe_name(attacker)
                    << " (guid: " << safe_guid(attacker) << ")"
                    << ", Level: " << safe_level(attacker)
                    << ", HP: " << safe_hp(attacker) << "/" << safe_maxhp(attacker)
                    << ", Distance: " << (dist >= 0 ? (std::ostringstream() << std::fixed << std::setprecision(1) << dist).str() : "?");
            }

            oss << ". ";
        }

        oss << "Your HP: " << (bot ? std::to_string(bot->GetHealth()) : "?") << "/" << (bot ? std::to_string(bot->GetMaxHealth()) : "?");
        oss << ", Mana: " << (bot ? std::to_string(bot->GetPower(POWER_MANA)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_MANA)) : "?");
        oss << ", Energy: " << (bot ? std::to_string(bot->GetPower(POWER_ENERGY)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_ENERGY)) : "?");
    }
    else
    {
        oss << "NOT IN COMBAT (" << combatType << " FIGHTER). ";
        
        // Check for health issues that might indicate environmental damage
        if (bot) {
            float healthPercent = (float)bot->GetHealth() / (float)bot->GetMaxHealth() * 100.0f;
            if (healthPercent < 90.0f) {
                oss << "WARNING: Your health is at " << (int)healthPercent << "% - you may be taking environmental damage! ";
            }
        }
        
        oss << "Your HP: " << (bot ? std::to_string(bot->GetHealth()) : "?") << "/" << (bot ? std::to_string(bot->GetMaxHealth()) : "?");
        oss << ", Mana: " << (bot ? std::to_string(bot->GetPower(POWER_MANA)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_MANA)) : "?");
        oss << ", Energy: " << (bot ? std::to_string(bot->GetPower(POWER_ENERGY)) : "?") << "/" << (bot ? std::to_string(bot->GetMaxPower(POWER_ENERGY)) : "?");
    }
    return oss.str();
}


std::string GetDetailedQuestInfo(Player* bot)
{
    std::ostringstream oss;
    
    bool hasActiveQuests = false;
    
    for (auto const& qs : bot->getQuestStatusMap())
    {
        uint32 questId = qs.first;
        QuestStatus status = qs.second.Status;
        
        // Skip abandoned, failed, or already rewarded quests
        if (status == QUEST_STATUS_NONE || status == QUEST_STATUS_FAILED || status == QUEST_STATUS_REWARDED)
            continue;
            
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) continue;
        
        if (!hasActiveQuests) {
            oss << "Active quests:\n";
            hasActiveQuests = true;
        }
        
        std::string statusText;
        switch (status) {
            case QUEST_STATUS_INCOMPLETE: statusText = "IN PROGRESS"; break;
            case QUEST_STATUS_COMPLETE: statusText = "READY TO TURN IN"; break;
            default: statusText = "UNKNOWN"; break;
        }
        
        oss << "\n**QUEST: " << quest->GetTitle() << "** (ID: " << questId << ") - " << statusText << "\n";
        oss << "Level: " << quest->GetQuestLevel() << " | XP Reward: " << quest->XPValue(bot->GetLevel()) << "\n";
        
        if (status == QUEST_STATUS_COMPLETE) {
            oss << "*** PRIORITY: FIND QUEST GIVER TO TURN IN THIS QUEST ***\n";
            
            // Find who can accept this quest turn-in
            std::vector<std::string> turnInNPCs;
            
            // Check creatures that can accept this quest
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(questId);
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr) {
                CreatureTemplate const* cTemplate = sObjectMgr->GetCreatureTemplate(itr->first);
                if (cTemplate) {
                    turnInNPCs.push_back(std::string("NPC: ") + cTemplate->Name);
                }
            }
            
            // Check game objects that can accept this quest
            QuestRelationBounds goQir = sObjectMgr->GetGOQuestInvolvedRelationBounds(questId);
            for (QuestRelations::const_iterator itr = goQir.first; itr != goQir.second; ++itr) {
                GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(itr->first);
                if (goTemplate) {
                    turnInNPCs.push_back(std::string("Object: ") + goTemplate->name);
                }
            }
            
            if (!turnInNPCs.empty()) {
                oss << "Turn in to: ";
                for (size_t i = 0; i < turnInNPCs.size(); ++i) {
                    oss << turnInNPCs[i];
                    if (i < turnInNPCs.size() - 1) oss << " OR ";
                }
                oss << "\n";
            }
        } else {
            // Quest is incomplete - show objectives
            oss << "Objectives to complete:\n";
            
            // Check kill objectives
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] != 0) {
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (requiredCount > 0) {
                        std::string targetName = "Unknown Target";
                        
                        if (quest->RequiredNpcOrGo[i] > 0) {
                            // It's a creature
                            CreatureTemplate const* cTemplate = sObjectMgr->GetCreatureTemplate(quest->RequiredNpcOrGo[i]);
                            if (cTemplate) {
                                targetName = std::string("Kill ") + cTemplate->Name;
                            }
                        } else {
                            // It's a game object (negative value)
                            GameObjectTemplate const* goTemplate = sObjectMgr->GetGameObjectTemplate(-quest->RequiredNpcOrGo[i]);
                            if (goTemplate) {
                                targetName = std::string("Use/Click ") + goTemplate->name;
                            }
                        }
                        
                        oss << " - " << targetName << ": " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " NEED " << (requiredCount - currentCount) << " MORE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Check item objectives
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredItemId[i] != 0) {
                    uint32 currentCount = bot->GetItemCount(quest->RequiredItemId[i], true);
                    uint32 requiredCount = quest->RequiredItemCount[i];
                    
                    if (requiredCount > 0) {
                        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]);
                        std::string itemName = itemTemplate ? itemTemplate->Name1 : "Unknown Item";
                        
                        oss << " - Collect " << itemName << ": " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " NEED " << (requiredCount - currentCount) << " MORE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Check exploration objectives
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i) {
                if (quest->RequiredNpcOrGo[i] == 0 && quest->RequiredNpcOrGoCount[i] > 0) {
                    // This might be an exploration or spell cast objective
                    uint32 currentCount = bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]);
                    uint32 requiredCount = quest->RequiredNpcOrGoCount[i];
                    
                    if (requiredCount > 0) {
                        oss << " - Exploration/Event objective: " << currentCount << "/" << requiredCount;
                        if (currentCount >= requiredCount) {
                            oss << " COMPLETE";
                        } else {
                            oss << " INCOMPLETE";
                        }
                        oss << "\n";
                    }
                }
            }
            
            // Show quest description for context
            if (!quest->GetObjectives().empty()) {
                oss << "Description: " << quest->GetObjectives() << "\n";
            }
        }
    }
    
    if (!hasActiveQuests) {
        oss << "No active quests. Look for quest givers with available quests or turn-ins ready!\n";
    }
    
    return oss.str();
}

std::vector<std::string> GetNearbyWaypoints(Player* bot, float radius = 200.0f)
{
    std::vector<std::string> wps;
    if (!bot) return wps;
    uint32 bot_map = bot->GetMapId();
    float bot_x = bot->GetPositionX();
    float bot_y = bot->GetPositionY();
    float bot_z = bot->GetPositionZ();

    auto nodes = sTravelNodeMap.getNodes();
    int idx = 0;
    for (TravelNode* node : nodes)
    {
        if (!node) continue;
        WorldPosition* pos = node->getPosition();
        if (!pos) continue;
        if (pos->GetMapId() != bot_map) continue;
        float dx = pos->GetPositionX() - bot_x;
        float dy = pos->GetPositionY() - bot_y;
        float dz = pos->GetPositionZ() - bot_z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > radius) continue;
        wps.push_back(fmt::format("Node #{} '{}' ({:.1f}, {:.1f}, {:.1f}), distance: {:.1f}", idx, node->getName(), pos->GetPositionX(), pos->GetPositionY(), pos->GetPositionZ(), dist));
        ++idx;
    }
    return wps;
}

OllamaBotControlLoop::OllamaBotControlLoop() : WorldScript("OllamaBotControlLoop") {}

static std::unordered_map<uint64_t, time_t> nextTick;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    std::string* responseBuffer = static_cast<std::string*>(userp);
    size_t totalSize = size * nmemb;
    responseBuffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

static std::string QueryOllamaLLM(const std::string& prompt)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] Failed to initialize cURL.");
        return "";
    }

    // Constrain the reply. Unconstrained, the model answers in markdown prose and
    // the decision is discarded - roughly a quarter of them were lost that way.
    //
    // A full schema is much stronger than plain "json": it pins the command enum,
    // the parameter names, and their types, so a reply cannot be well-formed JSON
    // that is still unusable (an "attack" with empty params, or a guid as a string).
    // Each command carries different parameters, so the schema branches on the
    // command type rather than accepting any object.
    //
    // Caveat: Ollama's MLX runner accepts a schema and silently ignores it
    // (ollama/ollama#17013, #16563), while the GGUF/llama.cpp runner enforces it.
    // On an MLX model, set OllamaBotControl.StrictSchema = 0 to fall back to plain
    // "json" - or better, use the GGUF build of the same model.
    auto guidCmd = [](char const* name)
    {
        return nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"type",   {{"const", name}}},
                {"params", {{"type", "object"},
                            {"properties", {{"guid", {{"type", "integer"}}}}},
                            {"required", {"guid"}}}}
            }},
            {"required", {"type", "params"}}
        };
    };

    nlohmann::json commandSchema = {
        {"oneOf", {
            // move_to needs real coordinates, not a target
            {
                {"type", "object"},
                {"properties", {
                    {"type",   {{"const", "move_to"}}},
                    {"params", {{"type", "object"},
                                {"properties", {{"x", {{"type", "number"}}},
                                                {"y", {{"type", "number"}}},
                                                {"z", {{"type", "number"}}}}},
                                {"required", {"x", "y", "z"}}}}
                }},
                {"required", {"type", "params"}}
            },
            guidCmd("attack"),
            guidCmd("interact"),
            guidCmd("move_to_target"),
            // loot names the corpse it is looting, so a failure can be attributed
            // to a specific target rather than to "looting" in the abstract.
            {
                {"type", "object"},
                {"properties", {
                    {"type",   {{"const", "loot"}}},
                    {"params", {{"type", "object"},
                                {"properties", {{"guid", {{"type", "integer"}}}}},
                                {"required", {"guid"}}}}
                }},
                {"required", {"type", "params"}}
            },
            {
                {"type", "object"},
                {"properties", {
                    {"type",   {{"enum", {"accept_quest", "turn_in_quest"}}}},
                    {"params", {{"type", "object"},
                                {"properties", {{"id", {{"type", "integer"}}}}},
                                {"required", {"id"}}}}
                }},
                {"required", {"type", "params"}}
            }
        }}
    };

    nlohmann::json schema = {
        {"type", "object"},
        {"properties", {
            {"command",   commandSchema},
            {"reasoning", {{"type", "string"}}},
            {"say",       {{"type", "string"}}}
        }},
        {"required", {"command", "reasoning"}}
    };

    nlohmann::json requestData = {
        {"model",  g_OllamaBotControlModel},
        {"prompt", prompt},
        {"stream", false},
        {"format", g_OllamaBotBuddyStrictSchema ? schema : nlohmann::json("json")}
    };
    std::string requestDataStr = requestData.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string responseBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, g_OllamaBotControlUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestDataStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(requestDataStr.length()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] Failed to reach Ollama AI. cURL error: {}", curl_easy_strerror(res));
        return "";
    }

    std::stringstream ss(responseBuffer);
    std::string line, extracted;
    while (std::getline(ss, line))
    {
        try
        {
            nlohmann::json jsonResponse = nlohmann::json::parse(line);
            if (jsonResponse.contains("response"))
                extracted += jsonResponse["response"].get<std::string>();
        }
        catch (...) {}
    }
    return extracted;
}

static std::string BuildBotPrompt(Player* bot)
{
    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    if (!botAI) return "";

    AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
    AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();

    std::vector<std::string> groupInfo = GetGroupStatus(bot);

    std::string botName             = bot->GetName();
    uint32_t botLevel               = bot->GetLevel();
    uint8_t botGenderByte           = bot->getGender();
    std::string botAreaName         = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea): "UnknownArea";
    std::string botZoneName         = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone): "UnknownZone";
    std::string botMapName          = bot->GetMap() ? bot->GetMap()->GetMapName() : "UnknownMap";
    std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
    std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
    std::string botGender           = (botGenderByte == 0 ? "Male" : "Female");
    std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string botGroupStatus      = (bot->GetGroup() ? "In a group" : "Solo");
    uint32_t botGold                = bot->GetMoney() / 10000;
    

    std::ostringstream oss;
    oss << "Bot state summary:\n";
    oss << "Name: " << botName << "\n";
    oss << "Level: " << botLevel << "\n";
    oss << "Class: " << botClass << "\n";
    oss << "Race: " << botRace << "\n";
    oss << "Gender: " << botGender << "\n";
    oss << "Faction: " << botFaction << "\n";
    oss << "Gold: " << botGold << "\n";
    oss << "Area: " << botAreaName << "\n";
    oss << "Zone: " << botZoneName << "\n";
    oss << "Map: " << botMapName << "\n";
    oss << "Position: " << bot->GetPositionX() << " " << bot->GetPositionY() << " " << bot->GetPositionZ() << "\n";

    oss << GetCombatSummary(bot) << "\n\n";

    oss << "Your known spells:\n" << GetBotSpellInfo(bot) << "\n\n";

    oss << "Group status: " << botGroupStatus << "\n";
    if (!groupInfo.empty()) {
        oss << "Group members:\n";
        for (const auto& entry : groupInfo) oss << " - " << entry << "\n";
    }

    oss << GetDetailedQuestInfo(bot) << "\n";

    std::vector<std::string> losLocs = GetVisibleLocations(bot);
    std::vector<std::string> wps = GetNearbyWaypoints(bot);

    if (!losLocs.empty()) {
        oss << "Visible locations/objects in line of sight:\n";
        for (const auto& entry : losLocs) oss << " - " << entry << "\n";
        
        // Check for critical priorities and add warnings
        bool hasEnemies = false;
        bool hasNeutrals = false;
        bool hasQuestTargets = false;
        bool hasQuestTurnIns = false;
        bool hasLootableCorpses = false;
        bool hasDeadCreatures = false;
        
        for (const auto& entry : losLocs) {
            if (entry.find("ENEMY:") != std::string::npos && entry.find("DEAD") == std::string::npos) {
                hasEnemies = true; // Only count living enemies
            }
            if (entry.find("NEUTRAL:") != std::string::npos && entry.find("DEAD") == std::string::npos) {
                hasNeutrals = true; // Only count living neutrals
            }
            if (entry.find("[QUEST TARGET") != std::string::npos) {
                hasQuestTargets = true;
            }
            if (entry.find("[QUEST GIVER - TURN IN READY]") != std::string::npos) {
                hasQuestTurnIns = true;
            }
            if (entry.find("DEAD") != std::string::npos) {
                hasDeadCreatures = true;
                if (entry.find("LOOTABLE") != std::string::npos) {
                    hasLootableCorpses = true;
                }
            }
        }
        
        // Priority warnings in order of importance
        if (hasQuestTurnIns) {
            oss << "*** HIGHEST PRIORITY: QUEST TURN-INS AVAILABLE! Find NPCs marked with [QUEST GIVER - TURN IN READY] immediately! ***\n";
        }
        if (hasLootableCorpses) {
            oss << "*** CRITICAL: DEAD CREATURES TO LOOT! Use 'loot' command on ALL creatures marked 'DEAD' or 'DEAD (LOOTABLE)' - NEVER attack dead creatures! ***\n";
        }
        if (hasQuestTargets) {
            oss << "*** QUEST TARGETS AVAILABLE! Attack ONLY the LIVING creatures marked with [QUEST TARGET] to complete your objectives! ***\n";
        }
        if (hasEnemies) {
            oss << "*** WARNING: LIVING ENEMIES ARE VISIBLE! You should attack LIVING enemies for XP and to defend yourself! ***\n";
        }
        if (hasNeutrals && !hasQuestTargets) {
            oss << "*** NEUTRAL CREATURES VISIBLE: These may be needed for quest objectives! Check if they are LIVING and attack if needed for quests! ***\n";
        }
        if (hasDeadCreatures) {
            oss << "*** IMPORTANT: ANY DEAD CREATURES MUST BE LOOTED, NOT ATTACKED! Use loot command for all creatures with 'DEAD' status! ***\n";
        }
    }

    if (!wps.empty()) {
        oss << "Nearby navigation waypoints:\n";
        for (const auto& entry : wps) oss << " - " << entry << "\n";
    }

    std::vector<std::string> nearbyPlayers = GetVisiblePlayers(bot);
    if (!nearbyPlayers.empty()) {
        oss << "Visible players in area:\n";
        for (const auto& entry : nearbyPlayers) oss << " - " << entry << "\n";
    }

    if (!losLocs.empty() || !wps.empty()) {
        oss << "You must select one of these locations or waypoints to move to, interact with, accept or turn in quests, attack, loot, or any other action or choose a new unexplored spot.\n";
        oss << "COORDINATE CALCULATION RULES:\n";
        oss << " - YOUR POSITION: Use your current Position coordinates as reference point for all calculations\n";
        oss << " - TO MOVE TO TARGETS: Use their exact 'Position: X Y Z' coordinates OR calculate closer positions\n";
        oss << " - TO MOVE CLOSER: Calculate coordinates 70% of the way between your position and target\n";
        oss << " - TO EXPLORE: Use waypoint coordinates from navigation list OR calculate new exploration points\n";
        oss << " - DISTANCE THRESHOLDS: <5.0=attack/interact directly, >15.0=move closer using calculated coordinates\n";
        oss << " - COORDINATE MATH: You can add/subtract 5-20 units from any position to create tactical positioning\n";
        oss << "IMPORTANT: You can ONLY attack creatures/NPCs that are listed above in the visible locations. If your quest requires creatures that are NOT visible, you must move to find them using waypoints or exploration.\n";
    }

    oss << FormatPlayerMessagesPromptSegment(bot);

    std::vector<std::string> cmdHist = GetBotCommandHistory(bot);

    std::vector<std::string> reasoningHist = GetBotReasoningHistory(bot);


    // What you did and what actually happened. Real outcomes replace the old
    // block of warnings - the model can correct itself when it can see failure.
    oss << "\n" << BotBuddy::RecentActionsPrompt(bot, g_OllamaBotBuddyHistoryDepth) << "\n";

    if (g_EnableOllamaBotBuddyDebug)
    {
        std::string safeSnapshot = EscapeBracesForFmt(oss.str());
        LOG_INFO("server.loading", "[OllamaBotBuddy] Bot Snapshot for '{}': {}", botName, safeSnapshot);
    }

    oss << R"(You are playing a character in World of Warcraft. Decide your single next action.

Goal: level up and get better gear, mainly by taking and completing quests, killing things that give experience, and looting what you kill.

How to choose:
- Deal with immediate danger first: if you are low on health and in combat, retreat or heal before anything else.
- Prefer whatever advances a quest objective listed above.
- Only act on creatures, objects and NPCs that appear in your visible list, using the exact guid shown there.
- You must be standing next to something to interact with it or loot it. If you are not close enough, move to it first; the outcome of your last action will tell you if you were too far.
- Attack only living creatures. Loot only ones marked DEAD (LOOTABLE) - that mark means you killed it and it still has something on it. If no corpse is marked that way, there is nothing to loot, so go kill something instead.
- Read the outcomes of your last actions before choosing. If the same command already failed for the same reason, that reason has not gone away - choose a different command, not the same one again.

Reply with a single JSON object and nothing else, in exactly this shape:
{"command":{"type":"<one of: move_to_target, move_to, attack, interact, loot, accept_quest, turn_in_quest>","params":{}},"reasoning":"<one short sentence>","say":"<optional, what you say out loud>"}

params by command type:
  move_to      {"x":<float>,"y":<float>,"z":<float>}
  attack       {"guid":<guid from your visible list>}
  interact     {"guid":<guid from your visible list>}
  loot         {"guid":<guid of a corpse marked DEAD (LOOTABLE)>}
  accept_quest {"id":<quest id>}
  turn_in_quest{"id":<quest id>})";


    return oss.str();
}

namespace
{
    struct OllamaBotState
    {
        std::atomic<bool> busy { false };
        time_t lastRequest { 0 };
    };
    std::unordered_map<uint64_t, OllamaBotState> ollamaBotStates;
}

std::string EscapeBracesForFmt(const std::string& input) {
    std::string output;
    output.reserve(input.size() * 2); // Avoid lots of reallocs

    for (char c : input) {
        if (c == '{' || c == '}') {
            output.push_back(c); // first brace
            output.push_back(c); // second brace
        } else {
            output.push_back(c);
        }
    }
    return output;
}

void OllamaBotControlLoop::OnUpdate(uint32 /*diff*/)
{
    if (!g_EnableOllamaBotControl) return;

    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* bot = itr.second;
        if (!bot->IsInWorld()) continue;
        std::string botName = bot->GetName();

        // Only bots explicitly designated in OllamaBotControl.BotNames are placed
        // under LLM control; every other bot keeps its normal playerbot AI.
        if (g_OllamaBotControlBotNames.find(botName) == g_OllamaBotControlBotNames.end()) continue;

        // Clear the normal Playerbot AI
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (ai)
        {
            ai->ClearStrategies(BOT_STATE_COMBAT);
            ai->ClearStrategies(BOT_STATE_NON_COMBAT);
            ai->ClearStrategies(BOT_STATE_DEAD);
        } else {
            continue;
        }

        uint64_t guid = bot->GetGUID().GetRawValue();
        OllamaBotState& state = ollamaBotStates[guid];

        // Only process if not already waiting for LLM
        if (!state.busy)
        {
            state.busy = true;
            state.lastRequest = time(nullptr);

            std::string prompt = BuildBotPrompt(bot);

            if (g_EnableOllamaBotBuddyDebug)
            {
                //LOG_INFO("server.loading", "[OllamaBotBuddy] Sending prompt for bot '{}': {}", botName, prompt);
            }

            std::thread([bot, guid, prompt]() {
                auto started = std::chrono::steady_clock::now();
                std::string llmReply = QueryOllamaLLM(prompt);
                double latency = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count();

                if (g_EnableOllamaBotBuddyDebug)
                {
                    std::string safeJson = EscapeBracesForFmt(llmReply);
                    LOG_INFO("server.loading", "[OllamaBotBuddy] LLM reply for '{}':\n{}", bot->GetName(), safeJson);
                }

                BotBuddy::ActionRecord record;

                if (llmReply.empty())
                {
                    record.command = "none";
                    record.outcome = "the model returned nothing";
                }
                else
                {
                    std::string jsonOnly = ExtractFirstJsonObject(llmReply);
                    if (jsonOnly.empty())
                    {
                        record.command = "none";
                        record.outcome = "reply was not valid JSON";
                        LOG_ERROR("server.loading", "[OllamaBotBuddy] No valid JSON object found in LLM reply: {}", llmReply);
                    }
                    else
                    {
                        try
                        {
                            auto root = nlohmann::json::parse(jsonOnly);
                            record.command   = root.value("command", nlohmann::json::object())
                                                   .value("type", std::string("none"));
                            record.params    = root.value("command", nlohmann::json::object())
                                                   .value("params", nlohmann::json::object()).dump();
                            record.reasoning = root.value("reasoning", std::string());
                        }
                        catch (...) {}

                        // Actions report their real outcome through SetLastOutcome; anything
                        // that does not gets the plain success/failure of the call itself.
                        bool executed = ParseAndExecuteBotJson(bot, jsonOnly);

                        bool outSucceeded = executed;
                        std::string outText;
                        if (!BotBuddy::PopPendingOutcome(bot, outSucceeded, outText))
                            outText = executed ? "" : "the action could not be carried out";

                        record.succeeded = outSucceeded;
                        record.outcome   = outText;

                        std::string updatedPrompt = BuildBotPrompt(bot);
                        SendBuddyBotStateToPlayer(bot, bot, updatedPrompt);
                    }
                }

                BotBuddy::PushAction(bot, record);
                if (g_EnableOllamaBotBuddyJournal)
                    BotBuddy::WriteJournal(bot, prompt, llmReply, record, latency);

                // Mark ready for the next request
                ollamaBotStates[guid].busy = false;
            }).detach();
        }
    }
}
