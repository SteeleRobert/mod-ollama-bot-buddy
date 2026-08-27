#include "mod-ollama-bot-buddy_config.h"
#include "Config.h"
#include "mod-ollama-bot-buddy_journal.h"
#include <sstream>
#include <algorithm>

std::unordered_set<std::string> g_OllamaBotControlBotNames;

uint32 g_OllamaBotBuddyHistoryDepth = 6;
bool g_EnableOllamaBotBuddyJournal = true;
bool g_OllamaBotBuddyStrictSchema = true;
int32 g_OllamaBotBuddyThink = -1;
bool g_EnableOllamaBotControl = true;
std::string g_OllamaBotControlUrl = "http://localhost:11434/api/generate";
std::string g_OllamaBotControlModel = "llama3.2:1b";
bool g_EnableOllamaBotBuddyDebug = false;
bool g_EnableBotBuddyAddon = false;

OllamaBotControlConfigWorldScript::OllamaBotControlConfigWorldScript() : WorldScript("OllamaBotControlConfigWorldScript") {}

void OllamaBotControlConfigWorldScript::OnStartup()
{
    g_EnableOllamaBotControl = sConfigMgr->GetOption<bool>("OllamaBotControl.Enable", true);
    g_OllamaBotControlUrl = sConfigMgr->GetOption<std::string>("OllamaBotControl.Url", "http://localhost:11434/api/generate");
    g_OllamaBotControlModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model", "llama3.2:1b");
    g_EnableOllamaBotBuddyDebug = sConfigMgr->GetOption<bool>("OllamaBotControl.Debug", false);
    g_EnableBotBuddyAddon = sConfigMgr->GetOption<bool>("OllamaBotControl.EnableBotBuddyAddon", false);

    // Comma-separated character names of bots to place under LLM control.
    // Empty (the default) means no bot is controlled, so the module stays inert
    // until a bot is explicitly designated.
    g_OllamaBotBuddyHistoryDepth = sConfigMgr->GetOption<uint32>("OllamaBotControl.HistoryDepth", 6);
    g_EnableOllamaBotBuddyJournal = sConfigMgr->GetOption<bool>("OllamaBotControl.Journal", true);
    g_OllamaBotBuddyStrictSchema = sConfigMgr->GetOption<bool>("OllamaBotControl.StrictSchema", true);
    g_OllamaBotBuddyThink = sConfigMgr->GetOption<int32>("OllamaBotControl.Think", -1);
    if (g_EnableOllamaBotBuddyJournal)
        BotBuddy::EnsureSchema();

    g_OllamaBotControlBotNames.clear();
    std::string names = sConfigMgr->GetOption<std::string>("OllamaBotControl.BotNames", "");
    std::stringstream ss(names);
    std::string name;
    while (std::getline(ss, name, ','))
    {
        name.erase(name.begin(), std::find_if(name.begin(), name.end(),
                   [](unsigned char c) { return !std::isspace(c); }));
        name.erase(std::find_if(name.rbegin(), name.rend(),
                   [](unsigned char c) { return !std::isspace(c); }).base(), name.end());
        if (!name.empty())
            g_OllamaBotControlBotNames.insert(name);
    }
}
