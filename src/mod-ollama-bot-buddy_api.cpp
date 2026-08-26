#include "mod-ollama-bot-buddy_api.h"
#include "mod-ollama-bot-buddy_journal.h"
#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_loop.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Log.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "Map.h"
#include "Event.h"
#include "QuestDef.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GossipDef.h"
#include "LootMgr.h"
#include <sstream>

// Constants for interaction and combat ranges
#define INTERACTION_DISTANCE 5.5f
#define ATTACK_DISTANCE 5.0f

namespace BotBuddyAI
{
    bool MoveTo(Player* bot, float x, float y, float z)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] MoveTo called for bot {} to position ({:.2f}, {:.2f}, {:.2f})", 
                bot->GetName(), x, y, z);
        }
        
        // Validate coordinates are reasonable 
        if (std::isnan(x) || std::isnan(y) || std::isnan(z) || 
            std::isinf(x) || std::isinf(y) || std::isinf(z)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid coordinates for MoveTo: ({}, {}, {})", x, y, z);
            }
            return false;
        }
        
        // Clear existing movement
        bot->GetMotionMaster()->Clear(false);
        bot->StopMoving();
        
        // Use direct movement for immediate response
        bot->GetMotionMaster()->MovePoint(0, x, y, z);
        
        // Also try using the bot's AI movement system as backup
        std::ostringstream coords;
        coords << x << ";" << y << ";" << z;
        Event event = Event("", coords.str());
        ai->DoSpecificAction("go", event);
        
        if (g_EnableOllamaBotBuddyDebug) {
            float distance = sqrt(pow(x - bot->GetPositionX(), 2) + 
                                pow(y - bot->GetPositionY(), 2) + 
                                pow(z - bot->GetPositionZ(), 2));
            LOG_INFO("server.loading", "[OllamaBotBuddy] MoveTo initiated, distance: {:.2f}", distance);
        }
        
        return true;
    }

    bool Attack(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        Unit* target = ObjectAccessor::GetUnit(*bot, guid);
        if (!target || !bot->IsWithinLOSInMap(target)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Target not found or not in LOS for guid: {}", guid.GetCounter());
            }
            BotBuddy::SetLastOutcome(bot, false,
                "you cannot see that target - it is gone, or something is between you and it. "
                "Pick a target from your visible list");
            return false;
        }

        // Check if target is dead - if so, refuse to attack and suggest looting instead
        if (!target->IsAlive()) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] REFUSING to attack dead target: {} - it should be looted, not attacked", target->GetName());
            }
            // You killed it. Saying so turns three wasted turns of re-attacking a
            // corpse into the loot that was the point of the fight.
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "{} is already dead - you killed it. Loot it with loot {{\"guid\":{}}}",
                target->GetName(), guid.GetCounter()));
            return false; // Explicitly refuse to attack dead creatures
        }

        // CRITICAL: Validate target before attacking to prevent friendly fire
        if (!bot->IsValidAttackTarget(target)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Invalid attack target: {} - not attackable", target->GetName());
            }
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "{} cannot be attacked. Choose a different target", target->GetName()));
            return false;
        }

        // Check if target is friendly - absolutely prevent attacking friendlies
        if (bot->IsFriendlyTo(target)) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Refusing to attack friendly target: {}", target->GetName());
            }
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "{} is friendly - you will not attack it. Choose a hostile or neutral target",
                target->GetName()));
            return false;
        }

        // Additional safety: check if target is in same group or guild
        if (Player* targetPlayer = target->ToPlayer()) {
            if (bot->IsInSameGroupWith(targetPlayer) || (bot->GetGuildId() != 0 && bot->GetGuildId() == targetPlayer->GetGuildId())) {
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Refusing to attack group/guild member: {}", target->GetName());
                }
                return false;
            }
        }

        // Check if target is GM
        if (target->ToPlayer() && target->ToPlayer()->IsGameMaster()) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Target is GM: {}", target->GetName());
            }
            return false;
        }

        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} attacking target {} (guid: {})", 
                bot->GetName(), target->GetName(), guid.GetCounter());
        }

        // Calculate ranges properly using AzerothCore standards
        float currentDistance = bot->GetExactDist2d(target);
        float meleeRange = bot->GetMeleeRange(target); // This includes combat reach calculation
        float combatReach = bot->GetCombatReach() + target->GetCombatReach();
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Combat distances - Current: {:.2f}, Melee: {:.2f}, CombatReach: {:.2f}", 
                currentDistance, meleeRange, combatReach);
        }

        // Set target in AI context immediately for proper behavior
        ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
        ai->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Set(guid);
        
        // Set selection and target properly
        bot->SetSelection(guid);
        bot->SetTarget(guid);
        
        // Face the target if in combat or close
        if (bot->IsInCombat() || currentDistance <= meleeRange + 5.0f) {
            bot->SetFacingToObject(target);
        }
        
        // Check if we need to move closer for melee combat
        if (currentDistance > meleeRange) {
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Moving to melee range - distance {:.2f} > meleeRange {:.2f}", 
                    currentDistance, meleeRange);
            }
            
            // Clear any existing movement and start chasing
            bot->GetMotionMaster()->Clear();
            
            // Use MoveChase with proper melee range - this should get the bot close enough to attack
            bot->GetMotionMaster()->MoveChase(target, 0.0f); // 0.0f means use default melee range
            
            // Change to combat engine to enable combat actions
            ai->ChangeEngine(BOT_STATE_COMBAT);
            
            // Also use playerbot AI movement action as backup
            Event moveEvent = Event("", "");
            ai->DoSpecificAction("reach melee", moveEvent);
            
            return true; // Movement initiated, attack will happen when in range
        }
        
        // We're in melee range - initiate combat
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] In melee range, starting combat");
        }
        
        // Change to combat engine to enable combat actions
        ai->ChangeEngine(BOT_STATE_COMBAT);
        
        // Face the target before attacking
        bot->SetFacingToObject(target);
        
        // Start auto-attack
        bot->Attack(target, true);
        
        // Use playerbot AI actions for better combat behavior
        Event event = Event("", "");
        bool result = false;
        
        // Try playerbot combat actions in order of preference
        if (ai->IsTank(bot)) {
            result = ai->DoSpecificAction("tank assist", event);
        } else {
            result = ai->DoSpecificAction("dps assist", event);
        }
        
        // Fallback to melee action if assist actions fail
        if (!result) {
            result = ai->DoSpecificAction("melee", event);
        }
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Combat initiated, playerbot action result: {}", 
                result ? "SUCCESS" : "FALLBACK_TO_MANUAL");
        }
        
        return true;
    }

    bool Interact(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        if (Creature* creature = ObjectAccessor::GetCreature(*bot, guid))
        {
            // Out of range: walk there, but report what actually happened rather than
            // claiming the interaction succeeded. The model only learns to close the
            // distance if the outcome it sees says the interaction did not happen.
            float distance = bot->GetDistance(creature);
            if (distance > INTERACTION_DISTANCE)
            {
                float angle = creature->GetAngle(bot);
                float destX = creature->GetPositionX() + cos(angle + M_PI) * 3.0f; // 3 yards away
                float destY = creature->GetPositionY() + sin(angle + M_PI) * 3.0f;
                float destZ = creature->GetPositionZ();

                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, destX, destY, destZ);

                BotBuddy::SetLastOutcome(bot, false, fmt::format(
                    "too far to interact with {} ({:.1f}y away, need {:.1f}y) - walking closer, retry when adjacent",
                    creature->GetName(), distance, (float)INTERACTION_DISTANCE));
                return false;
            }
            
            // Check if this is a quest giver and handle quest interaction properly
            if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
            {
                return InteractWithQuestGiver(bot, creature);
            }
            // Not everything you can stand next to is something you can talk to. A
            // wolf has no gossip and no quests, so "gossip hello" fails - and saying
            // only that it failed leaves the model to conclude it should try again.
            // Name the verb that does apply instead.
            bool talkable =
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP)       ||
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR)       ||
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER)      ||
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_FLIGHTMASTER) ||
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_INNKEEPER)    ||
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_BANKER)       ||
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_SPIRITHEALER) ||
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_AUCTIONEER)   ||
                creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_REPAIR);

            if (!talkable)
            {
                if (creature->isDead())
                    BotBuddy::SetLastOutcome(bot, false, fmt::format(
                        "{} is a corpse - you do not talk to it. Loot it with loot {{\"guid\":{}}}",
                        creature->GetName(), guid.GetCounter()));
                else if (bot->IsValidAttackTarget(creature))
                    BotBuddy::SetLastOutcome(bot, false, fmt::format(
                        "{} is a creature, not someone you can talk to. interact will never work "
                        "on it. Kill it instead: attack {{\"guid\":{}}}",
                        creature->GetName(), guid.GetCounter()));
                else
                    BotBuddy::SetLastOutcome(bot, false, fmt::format(
                        "{} has nothing to say and nothing to offer - leave it alone",
                        creature->GetName()));
                return false;
            }

            {
                // A real NPC: talk to it, and report if the gossip still would not open.
                bot->SetFacingToObject(creature);
                Event event = Event("", std::to_string(guid.GetCounter()));
                if (ai->DoSpecificAction("gossip hello", event))
                    return true;

                BotBuddy::SetLastOutcome(bot, false, fmt::format(
                    "could not open a conversation with {} - it may be busy or have nothing for "
                    "you right now. Try something else", creature->GetName()));
                return false;
            }
        }
        else if (GameObject* go = ObjectAccessor::GetGameObject(*bot, guid))
        {
            // Check interaction distance FIRST - move closer if needed  
            float distance = bot->GetDistance(go);
            float interactionDist = go->GetInteractionDistance();
            if (distance > interactionDist)
            {
                // Too far - move closer first
                if (g_EnableOllamaBotBuddyDebug) {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} moving to interact with {} at distance {:.1f}", 
                        bot->GetName(), go->GetName(), distance);
                }
                
                // Calculate a position close to the object but not directly on top
                float angle = go->GetAngle(bot);
                float destX = go->GetPositionX() + cos(angle + M_PI) * 2.0f; // 2 yards away
                float destY = go->GetPositionY() + sin(angle + M_PI) * 2.0f;
                float destZ = go->GetPositionZ();
                
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, destX, destY, destZ);
                BotBuddy::SetLastOutcome(bot, false, fmt::format(
                    "too far to use {} ({:.1f}y away, need {:.1f}y) - walking closer, retry when adjacent",
                    go->GetGOInfo()->name, distance, interactionDist));
                return false;
            }

            // Check if this is a quest giver game object
            if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
            {
                return InteractWithQuestGiver(bot, go);
            }
            else
            {
                // Use the bot's AI system to handle interaction with game objects
                bot->SetFacingToObject(go);
                Event event = Event("", go->GetGOInfo()->name);
                if (ai->DoSpecificAction("use", event))
                    return true;

                BotBuddy::SetLastOutcome(bot, false, fmt::format(
                    "{} would not open - it may be locked, empty, or need something you do not "
                    "have. Try something else", go->GetGOInfo()->name));
                return false;
            }
        }

        BotBuddy::SetLastOutcome(bot, false,
            "there is nothing here with that guid - it despawned, or you are too far for it to "
            "be loaded. Pick a guid from your visible list");
        return false;
    }

    // A quest giver whose quest you already hold, and have not finished, offers
    // nothing when you talk to it. Returning a bare false there sends the model
    // straight back to the same NPC - it talked to Sten Stoutarm 45 times in a row.
    // Name the quest and the objective that is short.
    std::string NothingToDoWithQuestGiver(Player* bot, WorldObject* questGiver)
    {
        for (auto const& qs : bot->getQuestStatusMap())
        {
            if (qs.second.Status != QUEST_STATUS_INCOMPLETE) continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(qs.first);
            if (!quest) continue;

            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (uint32 itemId = quest->RequiredItemId[i])
                {
                    uint32 have = bot->GetItemCount(itemId, true);
                    uint32 need = quest->RequiredItemCount[i];
                    if (have >= need) continue;

                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                    return fmt::format(
                        "{} has nothing for you - you already have \"{}\" and it is not finished. "
                        "You still need {} more {}. Go and get them, then come back",
                        questGiver->GetName(), quest->GetTitle(), need - have,
                        proto ? proto->Name1 : "of the quest item");
                }

                if (quest->RequiredNpcOrGo[i] > 0)
                {
                    uint32 have = bot->GetReqKillOrCastCurrentCount(qs.first, quest->RequiredNpcOrGo[i]);
                    uint32 need = quest->RequiredNpcOrGoCount[i];
                    if (have >= need) continue;

                    return fmt::format(
                        "{} has nothing for you - you already have \"{}\" and it is not finished. "
                        "You still need {} more. Go and do that, then come back",
                        questGiver->GetName(), quest->GetTitle(), need - have);
                }
            }
        }

        return fmt::format(
            "{} has no quest for you right now - nothing to take and nothing to hand in. "
            "Go and do something else", questGiver->GetName());
    }

    bool InteractWithQuestGiver(Player* bot, WorldObject* questGiver)
    {
        if (!bot || !questGiver) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        // Check interaction distance
        float qgDist = bot->GetDistance(questGiver);
        if (qgDist > INTERACTION_DISTANCE)
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "too far to talk to {} ({:.1f}y away, need {:.1f}y) - move closer first",
                questGiver->GetName(), qgDist, (float)INTERACTION_DISTANCE));
            return false;
        }

        // Face the quest giver
        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, questGiver, sPlayerbotAIConfig.sightDistance))
            bot->SetFacingToObject(questGiver);

        ObjectGuid guid = questGiver->GetGUID();
        
        // Prepare the quest menu for this quest giver
        bot->PrepareQuestMenu(guid);
        QuestMenu& questMenu = bot->PlayerTalkClass->GetQuestMenu();

        // Turn in what is finished, take what is offered - and keep the results,
        // because the caller's word is what the model hears. The old code discarded
        // both return values and reported success either way, so a failed accept
        // looked identical to a real one and the bot re-talked to the same NPC
        // 49 times, "succeeding" every time while its quest log stayed empty.
        std::vector<std::string> turnedIn;
        std::vector<std::string> accepted;

        for (uint32 i = 0; i < questMenu.GetMenuItemCount(); ++i)
        {
            QuestMenuItem const& menuItem = questMenu.GetItem(i);
            Quest const* quest = sObjectMgr->GetQuestTemplate(menuItem.QuestId);
            if (!quest) continue;

            QuestStatus status = bot->GetQuestStatus(menuItem.QuestId);

            if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, false))
            {
                if (TurnInQuest(bot, menuItem.QuestId))
                    turnedIn.push_back(quest->GetTitle());
            }
            else if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            {
                if (AcceptQuest(bot, menuItem.QuestId))
                    accepted.push_back(quest->GetTitle());
            }
        }

        if (!turnedIn.empty() || !accepted.empty())
        {
            auto join = [](std::vector<std::string> const& v)
            {
                std::string out;
                for (size_t i = 0; i < v.size(); ++i)
                    out += (i ? "\", \"" : "\"") + v[i];
                return out + "\"";
            };

            std::string summary;
            if (!turnedIn.empty())
                summary += fmt::format("turned in {}", join(turnedIn));
            if (!accepted.empty())
                summary += fmt::format("{}accepted {} - check your active quests for what it needs",
                                       turnedIn.empty() ? "" : "; ", join(accepted));

            BotBuddy::SetLastOutcome(bot, true, summary);
            return true;
        }

        // If no direct quest actions were available, try automatic gossip navigation
        if (Creature* creature = questGiver->ToCreature())
        {
            // Try to automatically navigate gossip menus for quest options
            if (AutoNavigateGossipForQuests(bot, creature))
            {
                return true;
            }
            
            // Nothing to hand in and nothing to take. Opening the gossip window
            // anyway would count as a success and teach the model to keep coming
            // back - the exact loop this path exists to break. Report why the
            // visit achieved nothing instead.
            BotBuddy::SetLastOutcome(bot, false, NothingToDoWithQuestGiver(bot, questGiver));
            return false;
        }
        else if (GameObject* go = questGiver->ToGameObject())
        {
            // Use game object interaction
            Event event = Event("", go->GetGOInfo()->name);
            if (ai->DoSpecificAction("use", event))
                return true;

            BotBuddy::SetLastOutcome(bot, false, NothingToDoWithQuestGiver(bot, questGiver));
            return false;
        }

        BotBuddy::SetLastOutcome(bot, false, NothingToDoWithQuestGiver(bot, questGiver));
        return false;
    }

    bool AutoNavigateGossipForQuests(Player* bot, Creature* creature)
    {
        if (!bot || !creature) return false;

        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;

        // Start gossip interaction
        WorldPacket packet(CMSG_GOSSIP_HELLO);
        packet << creature->GetGUID();
        bot->GetSession()->HandleGossipHelloOpcode(packet);

        // Wait a moment for the server to process
        if (!bot->PlayerTalkClass) return false;

        GossipMenu& gossipMenu = bot->PlayerTalkClass->GetGossipMenu();
        QuestMenu& questMenu = bot->PlayerTalkClass->GetQuestMenu();

        // First priority: Handle direct quest menus
        for (uint32 i = 0; i < questMenu.GetMenuItemCount(); ++i)
        {
            QuestMenuItem const& menuItem = questMenu.GetItem(i);
            Quest const* quest = sObjectMgr->GetQuestTemplate(menuItem.QuestId);
            if (!quest) continue;

            QuestStatus status = bot->GetQuestStatus(menuItem.QuestId);
            
            if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, false))
            {
                // Propagate the real result - claiming success for a failed turn-in
                // is how the model ends up talking to the same NPC forever.
                return TurnInQuest(bot, menuItem.QuestId);
            }
            else if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
            {
                return AcceptQuest(bot, menuItem.QuestId);
            }
        }

        // Second priority: Navigate gossip menu for quest-related options
        GossipMenuItemContainer const& gossipItems = gossipMenu.GetMenuItems();
        for (auto const& item : gossipItems)
        {
            GossipMenuItem const* gossipItem = &item.second;
            std::string message = gossipItem->Message;
            
            // Look for quest-related gossip options with enhanced keyword matching
            if (message.find("quest") != std::string::npos || 
                message.find("Quest") != std::string::npos ||
                message.find("mission") != std::string::npos ||
                message.find("task") != std::string::npos ||
                message.find("reward") != std::string::npos ||
                message.find("complete") != std::string::npos ||
                message.find("turn in") != std::string::npos ||
                message.find("finish") != std::string::npos)
            {
                // Select this gossip option
                WorldPacket selectPacket(CMSG_GOSSIP_SELECT_OPTION);
                selectPacket << creature->GetGUID();
                selectPacket << gossipMenu.GetMenuId();
                selectPacket << item.first;
                selectPacket << std::string("");
                bot->GetSession()->HandleGossipSelectOptionOpcode(selectPacket);
                
                if (g_EnableOllamaBotBuddyDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} selected gossip option: {}", 
                        bot->GetName(), message);
                }
                return true;
            }
        }

        return false;
    }

    bool HasQuestsAvailable(Player* bot, WorldObject* questGiver)
    {
        if (!bot || !questGiver) return false;

        // For creatures, check their quest relations directly (more efficient)
        if (Creature* creature = questGiver->ToCreature())
        {
            // Check for completable quests first (highest priority)
            QuestRelationBounds qir = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(creature->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    return true; // Has quest ready to turn in
                }
            }
            
            // Check for available quests (secondary priority)
            QuestRelationBounds qr = sObjectMgr->GetCreatureQuestRelationBounds(creature->GetEntry());
            for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
            {
                uint32 questId = itr->second;
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                    bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                {
                    return true; // Has quest available to accept
                }
            }
        }
        // For game objects, check quest relations
        else if (GameObject* go = questGiver->ToGameObject())
        {
            // Check for completable quests
            QuestRelationBounds qir = sObjectMgr->GetGOQuestInvolvedRelationBounds(go->GetEntry());
            for (QuestRelations::const_iterator itr = qir.first; itr != qir.second; ++itr)
            {
                uint32 questId = itr->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(questId))
                {
                    return true;
                }
            }
            
            // Check for available quests
            QuestRelationBounds qr = sObjectMgr->GetGOQuestRelationBounds(go->GetEntry());
            for (QuestRelations::const_iterator itr = qr.first; itr != qr.second; ++itr)
            {
                uint32 questId = itr->second;
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest && bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && 
                    bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                {
                    return true;
                }
            }
        }
        
        return false;
    }

    bool CastSpell(Player* bot, uint32 spellId, Unit* target)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo) return false;
        
        // Set the target in the AI context if provided
        if (target) {
            ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Set(target);
            
            // Check range requirements for the spell
            float spellRange = spellInfo->GetMaxRange(false);
            float currentDistance = bot->GetDistance(target);
            bool isMeleeSpell = spellRange <= ATTACK_DISTANCE;
            
            if (g_EnableOllamaBotBuddyDebug) {
                LOG_INFO("server.loading", "[OllamaBotBuddy] Casting spell {} on target at distance {:.1f}, spell range: {:.1f}", 
                    spellInfo->SpellName[0], currentDistance, spellRange);
            }
            
            // Handle positioning for spell casting
            Event moveEvent = Event("", "");
            if (isMeleeSpell && !bot->IsWithinMeleeRange(target)) {
                // Need to get into melee range for melee spells
                ai->DoSpecificAction("reach melee", moveEvent);
            } else if (!isMeleeSpell && currentDistance > spellRange) {
                // Need to get into spell range for ranged spells
                ai->DoSpecificAction("reach spell", moveEvent);
            } else if (!isMeleeSpell && currentDistance < 5.0f && ai->IsRanged(bot)) {
                // Ranged character too close - back away for better positioning
                ai->DoSpecificAction("flee", moveEvent);
            }
        }
        
        // Use the spell name directly as the action
        const char* spellName = spellInfo->SpellName[0];
        if (!spellName || !*spellName) return false;
        
        Event event = Event("", "");
        bool result = ai->DoSpecificAction(spellName, event);
        
        // If spell casting by name fails, try using spell ID
        if (!result && target) {
            // Try alternative approaches
            std::string spellIdStr = std::to_string(spellId);
            event = Event("", spellIdStr);
            result = ai->DoSpecificAction("cast", event);
        }
        
        if (g_EnableOllamaBotBuddyDebug) {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Spell cast result for {}: {}", 
                spellName, result ? "SUCCESS" : "FAILED");
        }
        
        return result;
    }

    bool Say(Player* bot, const std::string& msg)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle saying
        Event event = Event("", msg);
        return ai->DoSpecificAction("say", event);
    }

    bool FollowMaster(Player* bot)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle following
        Event event = Event("", "");
        return ai->DoSpecificAction("follow", event);
    }

    bool StopMoving(Player* bot)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        // Use the bot's AI system to handle stopping
        Event event = Event("", "");
        return ai->DoSpecificAction("stay", event);
    }

    // Accept a quest directly.
    //
    // The playerbots "accept quest" action cannot be used here: its Execute starts
    // with `requester = event.getOwner() ?: GetMaster()` and returns false when both
    // are null - and an autonomous bot has neither. Every call from this harness
    // failed on that line before doing anything, while the caller reported success.
    bool AcceptQuest(Player* bot, uint32 questId)
    {
        if (!bot || !bot->GetMap()) return false;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "there is no quest with id {} - use a quest id you were shown", questId));
            return false;
        }

        QuestStatus status = bot->GetQuestStatus(questId);
        if (status != QUEST_STATUS_NONE)
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "you already have \"{}\" in your log - no need to accept it again",
                quest->GetTitle()));
            return false;
        }

        // The quest giver must actually be here and close enough.
        Object* giver = nullptr;
        std::string giverName;
        float giverDist = 0.f;
        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* c = pair.second;
            if (!c || !c->IsAlive() || !c->hasQuest(questId)) continue;
            float d = bot->GetDistance(c);
            if (!giver || d < giverDist) { giver = c; giverName = c->GetName(); giverDist = d; }
        }
        if (!giver)
        {
            for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
            {
                GameObject* go = pair.second;
                if (!go || !go->hasQuest(questId)) continue;
                float d = bot->GetDistance(go);
                if (!giver || d < giverDist) { giver = go; giverName = go->GetName(); giverDist = d; }
            }
        }

        if (!giver)
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "nobody around here offers \"{}\" - find the right quest giver first",
                quest->GetTitle()));
            return false;
        }
        if (giverDist > INTERACTION_DISTANCE)
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "too far from {} to take \"{}\" ({:.1f}y away, need {:.1f}y) - move to them first",
                giverName, quest->GetTitle(), giverDist, (float)INTERACTION_DISTANCE));
            return false;
        }

        if (!bot->CanAddQuest(quest, false))
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "cannot take \"{}\" - your quest log is full. Finish or abandon something first",
                quest->GetTitle()));
            return false;
        }
        if (!bot->CanTakeQuest(quest, false))
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "you do not qualify for \"{}\" right now", quest->GetTitle()));
            return false;
        }

        bot->AddQuestAndCheckCompletion(quest, giver);

        BotBuddy::SetLastOutcome(bot, true, fmt::format(
            "accepted \"{}\" from {} - check your active quests for what it needs",
            quest->GetTitle(), giverName));

        if (g_EnableOllamaBotBuddyDebug)
            LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} accepted quest {}: {}",
                bot->GetName(), questId, quest->GetTitle());

        return true;
    }

    bool TurnInQuest(Player* bot, uint32 questId)
    {
        if (!bot) return false;
        
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai) return false;
        
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) return false;

        // Check if quest is ready to turn in
        if (bot->GetQuestRewardStatus(questId))
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "you already turned \"{}\" in - it is done. Do something else",
                quest->GetTitle()));
            return false;
        }
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "\"{}\" is not finished yet - complete its objectives before turning it in",
                quest->GetTitle()));
            return false;
        }
        
        if (!bot->CanRewardQuest(quest, false))
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "cannot turn \"{}\" in yet - a requirement is still missing (an item to hand "
                "over, or bag space for the reward)", quest->GetTitle()));
            return false;
        }
        
        // Find quest giver in range
        ObjectGuid questGiverGuid;
        Map* map = bot->GetMap();
        if (map)
        {
            for (auto const& pair : map->GetCreatureBySpawnIdStore())
            {
                Creature* creature = pair.second;
                if (!creature || !bot->IsWithinDistInMap(creature, INTERACTION_DISTANCE)) continue;
                if (!creature->hasInvolvedQuest(questId)) continue;
                
                questGiverGuid = creature->GetGUID();
                break;
            }
            
            // Also check game objects
            if (!questGiverGuid)
            {
                for (auto const& pair : map->GetGameObjectBySpawnIdStore())
                {
                    GameObject* go = pair.second;
                    if (!go || !bot->IsWithinDistInMap(go, INTERACTION_DISTANCE)) continue;
                    if (!go->hasInvolvedQuest(questId)) continue;
                    
                    questGiverGuid = go->GetGUID();
                    break;
                }
            }
        }
        
        if (!questGiverGuid)
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "nobody close enough takes \"{}\" - walk to the NPC the quest says to return "
                "to, then turn it in", quest->GetTitle()));
            return false;
        }
        
        // First, initiate quest completion dialog
        WorldPacket completePacket(CMSG_QUESTGIVER_COMPLETE_QUEST);
        completePacket << questGiverGuid << questId;
        completePacket.rpos(0);
        bot->GetSession()->HandleQuestgiverCompleteQuest(completePacket);
        
        // Handle quest rewards
        uint32 rewardIndex = 0;
        if (quest->GetRewChoiceItemsCount() > 1)
        {
            // Find the best reward using simple logic
            for (uint32 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
            {
                if (quest->RewardChoiceItemId[i])
                {
                    ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
                    if (item && bot->CanUseItem(item) == EQUIP_ERR_OK)
                    {
                        rewardIndex = i;
                        break; // Use first usable reward
                    }
                }
            }
        }
        
        // Complete the reward selection
        WorldPacket rewardPacket(CMSG_QUESTGIVER_CHOOSE_REWARD);
        rewardPacket << questGiverGuid << questId << rewardIndex;
        rewardPacket.rpos(0);
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(rewardPacket);
        
        std::string rewardNote;
        if (quest->GetRewChoiceItemsCount() > 0 && quest->RewardChoiceItemId[rewardIndex])
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[rewardIndex]))
                rewardNote = fmt::format(", taking {} as the reward", proto->Name1);

        BotBuddy::SetLastOutcome(bot, true, fmt::format(
            "turned in \"{}\"{}", quest->GetTitle(), rewardNote));

        if (g_EnableOllamaBotBuddyDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotBuddy] Bot {} turned in quest {}: {} with reward index {}", 
                bot->GetName(), questId, quest->GetTitle(), rewardIndex);
        }
        
        return true;
    }

    namespace
    {
        // A corpse this bot is allowed to loot: dead, flagged lootable, and either
        // tagged by us or by our group.
        bool BotMayLoot(Player* bot, Creature* c)
        {
            if (!c || !c->isDead()) return false;
            if (!c->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE)) return false;
            if (!c->hasLootRecipient()) return false;
            if (c->GetLootRecipient() == bot) return true;
            return c->GetLootRecipientGroup() && bot->GetGroup() == c->GetLootRecipientGroup();
        }

        Creature* NearestLootableCorpse(Player* bot, float radius)
        {
            Creature* best = nullptr;
            float bestDist = radius;
            for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
            {
                Creature* c = pair.second;
                if (!BotMayLoot(bot, c)) continue;
                float d = bot->GetDistance(c);
                if (d <= bestDist) { bestDist = d; best = c; }
            }
            return best;
        }
    }

    // Loot a corpse outright.
    //
    // The classic playerbots "loot" action cannot be used here: it is gated on the
    // LootObjectStack, which is only filled by AddLootAction under the loot strategy -
    // and the LLM harness calls ClearStrategies() on every engine, so that stack is
    // permanently empty and the action returns false forever. Likewise StoreLootAction
    // is a packet handler registered by that same strategy, so even a CMSG_LOOT would
    // never store anything. We therefore drain the corpse directly, which is also
    // synchronous - we know what was picked up and can say so in the outcome.
    bool LootCorpse(Player* bot, uint32 lowGuid)
    {
        if (!bot || !bot->GetMap()) return false;

        Creature* corpse = nullptr;
        if (lowGuid)
        {
            // Resolve the low guid the model saw in its visible list. ObjectGuid::Create
            // needs the creature entry too, which the model has no way to know, so scan.
            for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                if (pair.second && pair.second->GetGUID().GetCounter() == lowGuid)
                    { corpse = pair.second; break; }
            if (!corpse)
            {
                BotBuddy::SetLastOutcome(bot, false,
                    "nothing with that guid is here any more - it despawned; pick a target from your visible list");
                return false;
            }
            if (corpse->IsAlive())
            {
                BotBuddy::SetLastOutcome(bot, false, fmt::format(
                    "{} is alive ({}/{} hp), not a corpse - kill it before looting it",
                    corpse->GetName(), corpse->GetHealth(), corpse->GetMaxHealth()));
                return false;
            }
        }
        else
        {
            // No target named: fall back to the nearest corpse we own.
            corpse = NearestLootableCorpse(bot, 30.0f);
            if (!corpse)
            {
                BotBuddy::SetLastOutcome(bot, false,
                    "there is no corpse near you that you can loot - you only get loot from things you killed yourself, "
                    "and only while they are still marked DEAD (LOOTABLE) in your visible list");
                return false;
            }
        }

        float distance = bot->GetDistance(corpse);
        if (distance > INTERACTION_DISTANCE)
        {
            float angle = corpse->GetAngle(bot);
            bot->GetMotionMaster()->Clear();
            bot->GetMotionMaster()->MovePoint(0,
                corpse->GetPositionX() + cos(angle + M_PI) * 2.0f,
                corpse->GetPositionY() + sin(angle + M_PI) * 2.0f,
                corpse->GetPositionZ());

            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "too far to loot {} ({:.1f}y away, need {:.1f}y) - walking closer, retry when adjacent",
                corpse->GetName(), distance, (float)INTERACTION_DISTANCE));
            return false;
        }

        if (!corpse->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "{} has nothing left on it - it is already looted. Stop looting it and do something else",
                corpse->GetName()));
            return false;
        }

        if (!BotMayLoot(bot, corpse))
        {
            BotBuddy::SetLastOutcome(bot, false, fmt::format(
                "{} is not yours to loot - somebody else killed it. Kill your own targets to get loot",
                corpse->GetName()));
            return false;
        }

        if (bot->IsMounted()) bot->Dismount();
        if (bot->isMoving())  bot->StopMoving();

        Loot* loot = &corpse->loot;
        bot->SetLootGUID(corpse->GetGUID());
        loot->FillNotNormalLootFor(bot);
        loot->AddLooter(bot->GetGUID());

        std::vector<std::string> taken;
        uint32 gold = loot->gold;

        if (gold)
        {
            bot->ModifyMoney(gold);
            loot->gold = 0;
            loot->NotifyMoneyRemoved();
        }

        // GetMaxSlotInLootFor covers the normal items plus this player's quest/FFA/
        // conditional lists, so quest drops are picked up like anything else.
        uint32 maxSlot = loot->GetMaxSlotInLootFor(bot);
        std::string blocked;
        for (uint32 slot = 0; slot < maxSlot; ++slot)
        {
            InventoryResult msg = EQUIP_ERR_OK;
            LootItem* item = bot->StoreLootItem(uint8(slot), loot, msg);
            if (item && msg == EQUIP_ERR_OK)
            {
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid))
                    taken.push_back(item->count > 1
                        ? fmt::format("{}x {}", uint32(item->count), proto->Name1)
                        : proto->Name1);
            }
            else if (msg == EQUIP_ERR_INVENTORY_FULL)
            {
                blocked = "your bags are full";
            }
        }

        bot->SetLootGUID(ObjectGuid::Empty);
        loot->RemoveLooter(bot->GetGUID());

        // Same teardown the core does in WorldSession::DoLootRelease, so the corpse
        // stops advertising loot, decays on the looted timer, and turns skinnable.
        if (loot->isLooted())
        {
            corpse->AllLootRemovedFromCorpse();
            corpse->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            loot->clear();
        }

        if (taken.empty() && !gold)
        {
            BotBuddy::SetLastOutcome(bot, false, blocked.empty()
                ? fmt::format("{} had nothing you could take", corpse->GetName())
                : fmt::format("could not loot {} - {}", corpse->GetName(), blocked));
            return false;
        }

        std::string got;
        for (size_t i = 0; i < taken.size(); ++i)
            got += (i ? ", " : "") + taken[i];
        if (gold)
            got += fmt::format("{}{} copper", taken.empty() ? "" : ", ", gold);

        BotBuddy::SetLastOutcome(bot, true,
            fmt::format("looted {} from {}", got, corpse->GetName()));

        if (g_EnableOllamaBotBuddyDebug)
            LOG_INFO("server.loading", "[OllamaBotBuddy] {} looted {} from {}",
                bot->GetName(), got, corpse->GetName());

        return true;
    }

} // namespace BotBuddyAI

bool HandleBotControlCommand(Player* bot, const BotControlCommand& command)
{
    if (g_EnableOllamaBotBuddyDebug && bot)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] HandleBotControlCommand for '{}', type {}", bot->GetName(), int(command.type));
        LOG_INFO("server.loading", "[OllamaBotBuddy] ================================================================================================");
    }
    if (!bot) return false;
    switch (command.type)
    {
        case BotControlCommandType::MoveTo:
            if (command.args.size() >= 3)
            {
                float x = std::stof(command.args[0]);
                float y = std::stof(command.args[1]);
                float z = std::stof(command.args[2]);
                return BotBuddyAI::MoveTo(bot, x, y, z);
            }
            break;
        case BotControlCommandType::Attack:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
                try {
                    lowGuid = std::stoul(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[0]);
                    return false;
                }

                // Try to find the Creature by LowGuid first
                Creature* creatureTarget = nullptr;
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (!c) continue;
                    if (c->GetGUID().GetCounter() == lowGuid)
                    {
                        creatureTarget = c;
                        break;
                    }
                }

                if (creatureTarget)
                {
                    // Use the actual GUID from the creature, never reconstruct!
                    return BotBuddyAI::Attack(bot, creatureTarget->GetGUID());
                }

                // If not found, try Player
                ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                if (playerTarget)
                {
                    return BotBuddyAI::Attack(bot, playerTarget->GetGUID());
                }

                LOG_INFO("server.loading", "[OllamaBotBuddy] Could not find target with lowGuid {}", lowGuid);
                return false;
            }
            break;
        case BotControlCommandType::MoveToTarget:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
                try { lowGuid = std::stoul(command.args[0]); }
                catch (...) { return false; }

                WorldObject* target = nullptr;
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                    if (pair.second && pair.second->GetGUID().GetCounter() == lowGuid)
                        { target = pair.second; break; }
                if (!target)
                    for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
                        if (pair.second && pair.second->GetGUID().GetCounter() == lowGuid)
                            { target = pair.second; break; }

                if (!target)
                {
                    BotBuddy::SetLastOutcome(bot, false, "no such target in this area");
                    return false;
                }

                float dist = bot->GetDistance(target);
                if (dist <= INTERACTION_DISTANCE)
                {
                    BotBuddy::SetLastOutcome(bot, true, fmt::format(
                        "already next to {} ({:.1f}y) - you can interact or attack now",
                        target->GetName(), dist));
                    return true;
                }

                // Stand just inside interaction range on the near side of the target.
                float angle = target->GetAngle(bot);
                float destX = target->GetPositionX() + cos(angle) * 3.0f;
                float destY = target->GetPositionY() + sin(angle) * 3.0f;
                float destZ = target->GetPositionZ();
                bot->GetMotionMaster()->Clear();
                bot->GetMotionMaster()->MovePoint(0, destX, destY, destZ);
                BotBuddy::SetLastOutcome(bot, true, fmt::format(
                    "walking to {} ({:.1f}y away)", target->GetName(), dist));
                return true;
            }
            break;
        case BotControlCommandType::Interact:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
                try {
                    lowGuid = std::stoul(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[0]);
                    return false;
                }
                Creature* creatureTarget = nullptr;
                GameObject* goTarget = nullptr;

                // Find creature by LowGuid
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (!c) continue;
                    if (c->GetGUID().GetCounter() == lowGuid)
                    {
                        creatureTarget = c;
                        break;
                    }
                }

                if (creatureTarget)
                {
                    return BotBuddyAI::Interact(bot, creatureTarget->GetGUID());
                }

                // Find gameobject by LowGuid
                for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
                {
                    GameObject* go = pair.second;
                    if (!go) continue;
                    if (go->GetGUID().GetCounter() == lowGuid)
                    {
                        goTarget = go;
                        break;
                    }
                }

                if (goTarget)
                {
                    return BotBuddyAI::Interact(bot, goTarget->GetGUID());
                }

                LOG_INFO("server.loading", "[OllamaBotBuddy] Could not find interact target with lowGuid {}", lowGuid);
                return false;
            }
            break;
        case BotControlCommandType::CastSpell:
            if (!command.args.empty())
            {
                uint32 spellId = 0;
                try {
                    spellId = std::stoi(command.args[0]);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for spellId '{}'", command.args[0]);
                    return false;
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for spellId '{}'", command.args[0]);
                    return false;
                }
                Unit* target = nullptr;
                if (command.args.size() > 1)
                {
                    uint32 lowGuid = 0;
        try {
            lowGuid = std::stoul(command.args[1]);
        } catch (const std::invalid_argument& e) {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid argument for lowGuid '{}'", command.args[1]);
            return false;
        } catch (const std::out_of_range& e) {
            LOG_ERROR("server.loading", "[OllamaBotBuddy] Out of range value for lowGuid '{}'", command.args[1]);
            return false;
        }
                    // Try to find creature by lowGuid
                    for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                    {
                        Creature* c = pair.second;
                        if (c && c->GetGUID().GetCounter() == lowGuid)
                        {
                            target = c;
                            break;
                        }
                    }
                    // Try to find player by lowGuid if not found
                    if (!target)
                    {
                        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                        Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                        if (playerTarget) target = playerTarget;
                    }
                }
                else
                {
                    target = bot; // Use bot itself as the target if no guid provided
                }
                return BotBuddyAI::CastSpell(bot, spellId, target);
            }
            break;
        case BotControlCommandType::Say:
            if (!command.args.empty())
            {
                return BotBuddyAI::Say(bot, command.args[0]);
            }
            break;
        case BotControlCommandType::Follow:
            return BotBuddyAI::FollowMaster(bot);
        case BotControlCommandType::Stop:
            return BotBuddyAI::StopMoving(bot);
        case BotControlCommandType::AcceptQuest:
            if (!command.args.empty())
            {
                uint32 questId = std::stoi(command.args[0]);
                return BotBuddyAI::AcceptQuest(bot, questId);
            }
            break;
        case BotControlCommandType::TurnInQuest:
            if (!command.args.empty())
            {
                uint32 questId = std::stoi(command.args[0]);
                return BotBuddyAI::TurnInQuest(bot, questId);
            }
            break;
        case BotControlCommandType::Loot:
        {
            // Optional target: the model names a corpse, or we take the nearest one.
            uint32 lootGuid = 0;
            if (!command.args.empty())
            {
                try { lootGuid = std::stoul(command.args[0]); }
                catch (const std::exception&) {
                    LOG_ERROR("server.loading", "[OllamaBotBuddy] Invalid loot guid '{}'", command.args[0]);
                }
            }
            return BotBuddyAI::LootCorpse(bot, lootGuid);
        }
        default:
            break;
    }
    return false;
}


bool ParseBotControlCommand(Player* bot, const std::string& commandStr)
{
    if (g_EnableOllamaBotBuddyDebug && bot)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] ParseBotControlCommand for '{}': {}", bot->GetName(), commandStr);
    }
    std::istringstream iss(commandStr);
    std::string cmd;
    iss >> cmd;
    if (cmd == "move")
    {
        std::string to;
        iss >> to;
        if (to != "to") return false;
        float x, y, z;
        iss >> x >> y >> z;
        BotControlCommand command = {BotControlCommandType::MoveTo, {std::to_string(x), std::to_string(y), std::to_string(z)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "attack")
    {
        std::string guid;
        iss >> guid;
        BotControlCommand command = {BotControlCommandType::Attack, {guid}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "interact")
    {
        std::string guid;
        iss >> guid;
        BotControlCommand command = {BotControlCommandType::Interact, {guid}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "say")
    {
        std::string msg;
        std::getline(iss, msg);
        BotControlCommand command = {BotControlCommandType::Say, {msg}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "loot")
    {
        BotControlCommand command = {BotControlCommandType::Loot, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "follow")
    {
        BotControlCommand command = {BotControlCommandType::Follow, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "stop")
    {
        BotControlCommand command = {BotControlCommandType::Stop, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "acceptquest")
    {
        uint32 questId;
        iss >> questId;
        BotControlCommand command = {BotControlCommandType::AcceptQuest, {std::to_string(questId)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "turninquest")
    {
        uint32 questId;
        iss >> questId;
        BotControlCommand command = {BotControlCommandType::TurnInQuest, {std::to_string(questId)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "spell")
    {
        uint32 spellId;
        iss >> spellId;
        std::string targetGuid;
        iss >> targetGuid;
        BotControlCommand command;
        if (!targetGuid.empty())
        {
            command = {BotControlCommandType::CastSpell, {std::to_string(spellId), targetGuid}};
        }
        else
        {
            command = {BotControlCommandType::CastSpell, {std::to_string(spellId)}};
        }
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    return false;
}

std::string FormatCommandString(const BotControlCommand& command)
{
    std::ostringstream ss;
    switch (command.type)
    {
        case BotControlCommandType::MoveTo:
            ss << "move to";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Attack:
            ss << "attack";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Interact:
            ss << "interact";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::CastSpell:
            ss << "cast";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Loot:
            ss << "loot";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Follow:
            ss << "follow";
            break;
        case BotControlCommandType::Say:
            ss << "say";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::AcceptQuest:
            ss << "acceptquest";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::TurnInQuest:
            ss << "turninquest";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Stop:
            ss << "stop";
            break;
        default:
            ss << "unknown command";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
    }
    return ss.str();
}


