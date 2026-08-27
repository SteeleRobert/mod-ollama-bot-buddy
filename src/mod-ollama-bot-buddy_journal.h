#pragma once
/*
 * Action journal for the LLM bot harness.
 *
 * Two jobs:
 *   1. Record the outcome of the action the model just took, so the NEXT prompt can
 *      tell it what actually happened. Without this the model is told every command
 *      succeeded and can never correct itself.
 *   2. Persist every decision - prompt, raw reply, chosen command, real outcome and
 *      latency - to the database so runs can be reviewed and compared after the fact.
 */
#include "Player.h"
#include <string>
#include <deque>

namespace BotBuddy
{
    struct ActionRecord
    {
        std::string command;    // e.g. "interact"
        std::string params;     // compact JSON of the params
        std::string reasoning;  // the model's own justification
        bool        succeeded = false;
        std::string outcome;    // what actually happened, in plain language
    };

    /// Record what really happened when an action ran. Called from the action
    /// implementations themselves, including on the failure paths.
    void SetLastOutcome(Player* bot, bool succeeded, std::string const& outcome);

    /// Push a completed decision onto the bot's rolling history.
    void PushAction(Player* bot, ActionRecord record);

    /// The last `count` actions, oldest first, rendered for the prompt.
    std::string RecentActionsPrompt(Player* bot, uint32 count);

    /// Persist one decision. Fire-and-forget; never blocks the world thread.
    void WriteJournal(Player* bot, std::string const& prompt, std::string const& reply,
                      ActionRecord const& record, double latencySeconds);

    /// Create the journal table if it does not exist.
    void EnsureSchema();

    /// Outcome of the action currently being executed (set by SetLastOutcome).
    bool PopPendingOutcome(Player* bot, bool& succeeded, std::string& outcome);
}
