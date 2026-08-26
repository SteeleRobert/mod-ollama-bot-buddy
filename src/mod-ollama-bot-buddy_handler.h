#include "ScriptMgr.h"
#include <string>
#include <Group.h>
#include <Channel.h>

extern std::unordered_map<uint64_t, std::deque<std::pair<std::string, std::string>>> botPlayerMessages;
extern std::mutex botPlayerMessagesMutex;

class BotBuddyChatHandler : public PlayerScript
{
public:
    BotBuddyChatHandler() : PlayerScript("BotBuddyChatHandler") {}

    // This core exposes chat as OnPlayerCanUseChat (bool) rather than the older
    // void OnPlayerChat hooks. We observe only and always allow the message through.
    [[nodiscard]] bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg) override;
    [[nodiscard]] bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override;
    [[nodiscard]] bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override;

private:
    void ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel = nullptr);
};
