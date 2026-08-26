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

        // Identical repeated failures get collapsed rather than listed one by one.
        // Six separate lines of the same failure read as six unlucky attempts and
        // invite the model to try a seventh; "x6" reads as a fact about the world.
        auto sameAttempt = [](ActionRecord const& a, ActionRecord const& b)
        {
            return a.command == b.command && a.params == b.params && a.outcome == b.outcome;
        };

        std::string out = "Your last actions, oldest first:\n";
        for (size_t i = start; i < dq.size(); )
        {
            ActionRecord const& r = dq[i];
            size_t j = i + 1;
            while (j < dq.size() && sameAttempt(r, dq[j])) ++j;
            size_t repeats = j - i;

            out += "- " + r.command;
            if (!r.params.empty()) out += " " + r.params;
            out += r.succeeded ? "  -> worked" : "  -> DID NOT WORK";
            if (!r.outcome.empty()) out += ": " + r.outcome;
            if (repeats > 1)
                out += "  [you did this " + std::to_string(repeats) + " times in a row"
                     + (r.succeeded ? "]" : ", with the same result every time]");
            out += "\n";
            // Only the most recent justification is worth showing; the rest are
            // restatements of it and just crowd the window.
            if (!r.reasoning.empty())
                out += "  (you said: " + dq[j - 1].reasoning + ")\n";

            i = j;
        }

        // A failure the model has already repeated needs to be stated as a
        // conclusion, not left for it to infer from a list it can rationalise past.
        ActionRecord const& last = dq.back();
        if (!last.succeeded)
        {
            size_t streak = 0;
            for (size_t i = dq.size(); i-- > 0; )
            {
                if (dq[i].succeeded || !sameAttempt(dq[i], last)) break;
                ++streak;
            }
            if (streak >= 2)
            {
                out += "\nSTOP: \"" + last.command;
                if (!last.params.empty()) out += " " + last.params;
                out += "\" has now failed " + std::to_string(streak)
                     + " times in a row for the same reason. It will keep failing. "
                       "Do not choose it again this turn - the reason it failed is a "
                       "fact about the world, not bad luck. Pick a different command.\n";
            }
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
