#include "mod-ollama-bot-buddy_journal.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <mutex>
#include <unordered_map>

namespace BotBuddy
{
    namespace
    {
        std::mutex g_mutex;
        std::unordered_map<uint64, std::deque<ActionRecord>> g_history;
        std::unordered_map<uint64, std::pair<bool, std::string>> g_pending;
        constexpr size_t HISTORY_CAP = 12;
    }

    void SetLastOutcome(Player* bot, bool succeeded, std::string const& outcome)
    {
        if (!bot) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending[bot->GetGUID().GetRawValue()] = {succeeded, outcome};
    }

    bool PopPendingOutcome(Player* bot, bool& succeeded, std::string& outcome)
    {
        if (!bot) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_pending.find(bot->GetGUID().GetRawValue());
        if (it == g_pending.end()) return false;
        succeeded = it->second.first;
        outcome   = it->second.second;
        g_pending.erase(it);
        return true;
    }

    void PushAction(Player* bot, ActionRecord record)
    {
        if (!bot) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& dq = g_history[bot->GetGUID().GetRawValue()];
        dq.push_back(std::move(record));
        while (dq.size() > HISTORY_CAP)
            dq.pop_front();
    }

    std::string RecentActionsPrompt(Player* bot, uint32 count)
    {
        if (!bot || !count) return "";
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_history.find(bot->GetGUID().GetRawValue());
        if (it == g_history.end() || it->second.empty())
            return "You have not acted yet.\n";

        auto const& dq = it->second;
        size_t start = dq.size() > count ? dq.size() - count : 0;

        std::string out = "Your last actions, oldest first:\n";
        for (size_t i = start; i < dq.size(); ++i)
        {
            ActionRecord const& r = dq[i];
            out += "- " + r.command;
            if (!r.params.empty()) out += " " + r.params;
            out += r.succeeded ? "  -> worked" : "  -> DID NOT WORK";
            if (!r.outcome.empty()) out += ": " + r.outcome;
            out += "\n";
            if (!r.reasoning.empty()) out += "  (you said: " + r.reasoning + ")\n";
        }
        return out;
    }

    void EnsureSchema()
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_ollama_bot_buddy_journal` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`bot_guid` BIGINT UNSIGNED NOT NULL,"
            "`bot_name` VARCHAR(24) NOT NULL,"
            "`ts` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`command` VARCHAR(32) NOT NULL,"
            "`params` TEXT,"
            "`reasoning` TEXT,"
            "`succeeded` TINYINT(1) NOT NULL DEFAULT 0,"
            "`outcome` TEXT,"
            "`latency_ms` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`prompt` MEDIUMTEXT,"
            "`reply` MEDIUMTEXT,"
            "PRIMARY KEY (`id`), KEY `bot_ts` (`bot_guid`,`ts`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    }

    void WriteJournal(Player* bot, std::string const& prompt, std::string const& reply,
                      ActionRecord const& record, double latencySeconds)
    {
        if (!bot) return;

        std::string name = bot->GetName();
        std::string params = record.params, reasoning = record.reasoning,
                    outcome = record.outcome, p = prompt, r = reply, cmd = record.command;
        CharacterDatabase.EscapeString(name);
        CharacterDatabase.EscapeString(params);
        CharacterDatabase.EscapeString(reasoning);
        CharacterDatabase.EscapeString(outcome);
        CharacterDatabase.EscapeString(cmd);
        CharacterDatabase.EscapeString(p);
        CharacterDatabase.EscapeString(r);

        CharacterDatabase.Execute(
            "INSERT INTO mod_ollama_bot_buddy_journal "
            "(bot_guid, bot_name, command, params, reasoning, succeeded, outcome, latency_ms, prompt, reply) "
            "VALUES (" + std::to_string(bot->GetGUID().GetRawValue()) + ",'" + name + "','" + cmd + "','" +
            params + "','" + reasoning + "'," + (record.succeeded ? "1" : "0") + ",'" + outcome + "'," +
            std::to_string((uint32)(latencySeconds * 1000)) + ",'" + p + "','" + r + "')");
    }
}
