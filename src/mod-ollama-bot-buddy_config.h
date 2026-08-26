#pragma once
#include "ScriptMgr.h"
#include <string>
#include <unordered_set>

extern bool g_EnableOllamaBotControl;
extern std::string g_OllamaBotControlUrl;
extern std::string g_OllamaBotControlModel;
extern bool g_EnableOllamaBotBuddyDebug;
extern bool g_EnableBotBuddyAddon;
// Bots placed under LLM control, by character name. Empty = no bots controlled.
extern std::unordered_set<std::string> g_OllamaBotControlBotNames;
// How many past actions (with their real outcomes) to show the model.
extern uint32 g_OllamaBotBuddyHistoryDepth;
// Persist every decision to mod_ollama_bot_buddy_journal.
extern bool g_EnableOllamaBotBuddyJournal;
// Constrain replies with a full JSON schema. Ollama's MLX runner ignores schemas
// (ollama/ollama#17013); set false there to fall back to plain "json".
extern bool g_OllamaBotBuddyStrictSchema;

class OllamaBotControlConfigWorldScript : public WorldScript
{
public:
    OllamaBotControlConfigWorldScript();
    void OnStartup() override;
};
