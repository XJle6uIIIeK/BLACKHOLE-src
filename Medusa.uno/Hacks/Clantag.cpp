#include "Clantag.h"

#include "../Interfaces.h"
#include "../Memory.h"

#include "../SDK/ConVar.h"
#include "../SDK/Cvar.h"
#include "../SDK/Entity.h"
#include "../SDK/Engine.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/LocalPlayer.h"
#include "../SDK/NetworkChannel.h"
#include "../SDK/PlayerResource.h"
#include "../GameData.h"




void BLACKHOLEClanTag() noexcept {

    const char* clantagBLACKHOLE[] = {

    " | ",
    " |8 ",
    " B ",
    " BL ",
    " BL4 ",
    " BLA ",
    " BLAC ",
    " BLAC| ",
    " BLAC|< ",
    " BLACK ",
    " BLACK| ",
    " BLACK|- ",
    " BLACK|-| ",
    " BLACKH ",
    " BLACKH() ",
    " BLACKHOL ",
    " BLACKHOLE ",
    " BLACKHOLE ",
    " BLACKHOL ",
    " BLACKH() ",
    " BLACKH ",
    " BLACK|-| ",
    " BLACK|- ",
    " BLACK| ",
    " BLACK ",
    " BLAC|< ",
    " BLAC| ",
    " BLAC ",
    " BLA ",
    " BL4 ",
    " BL ",
    " |8 ",
    " | ",

    };

    float latency = interfaces->engine->getNetworkChannel()->getLatency(0) + interfaces->engine->getNetworkChannel()->getLatency(1);

    int serverTime = static_cast<int>(((memory->globalVars->currenttime / 0.296875f) + 6.60925f - 0.07f) - (interfaces->engine->getNetworkChannel()->getLatency(0) + interfaces->engine->getNetworkChannel()->getLatency(1)));
    memory->setClanTag(std::string(clantagBLACKHOLE[serverTime % 33]).append("\n").c_str(), clantagBLACKHOLE[serverTime % 33]);
}

void gamesenzeClanTag() noexcept {

    const char* clantagGamesense[] = {

        "sense          ",
        "ense           ",
        "nse            ",
        "se             ",
        "e              ",
        "               ",
        "             ga",
        "            gam",
        "           game",
        "          games",
        "         gamese",
        "        gamesen",
        "       gamesens",
        "      gamesense",
        "      gamesense",
        "     gamesense ",
        "     gamesense ",
        "    gamesense  ",
        "    gamesense  ",
        "   gamesense   ",
        "   gamesense   ",
        "  gamesense    ",
        "  gamesense    ",
        " gamesense     ",
        " gamesense     ",
        "gamesense      ",
        "gamesense      ",
        "amesense       ",
        "mesense        ",
        "esense         "

    };

    float latency = interfaces->engine->getNetworkChannel()->getLatency(0) + interfaces->engine->getNetworkChannel()->getLatency(1);

    int serverTime = static_cast<int>(((memory->globalVars->currenttime / 0.296875f) + 6.60925f - 0.07f) - (interfaces->engine->getNetworkChannel()->getLatency(0) + interfaces->engine->getNetworkChannel()->getLatency(1)));
    memory->setClanTag(clantagGamesense[serverTime % 30], clantagGamesense[serverTime % 30]);
}

void ClanTagStealer::update() noexcept {
    Clan::players.clear();
    if (config->clanCfg.mode != 10) return;
    if (!localPlayer) return;
    std::vector<std::reference_wrapper<const PlayerData>> playersOrdered{ GameData::players().begin(), GameData::players().end() };
    for (const PlayerData& player : playersOrdered) {
        Clan::players.push_back({ player.index , player.name });
    }
}

static  int stealFromIdx = -1;
static bool stealEnabled = false;

void Clan::update(bool reset, bool update) noexcept
{
    if (!localPlayer) return;

    static auto clanId = interfaces->cvar->findVar("cl_clanid");
    static bool wasEnabled = false;

    if (wasEnabled && !config->clanCfg.mode)
    {
        interfaces->engine->clientCmdUnrestricted(("cl_clanid " + std::to_string(clanId->getInt())).c_str());
        wasEnabled = false;
        return;
    }

    if (!config->clanCfg.mode) return;

    wasEnabled = config->clanCfg.mode;

    if (reset) {
        interfaces->engine->clientCmdUnrestricted(("cl_clanid " + std::to_string(clanId->getInt())).c_str());
        return;
    }

    static float prevTime = 0.f;
    static float timeToUpdate = 0.f;
    float realTime = memory->globalVars->realtime;

    static auto steal = []() {
        if (!stealEnabled || stealFromIdx == -1) return;

        if (!localPlayer) {
            stealFromIdx = -1;
            stealEnabled = false;
            return;
        }

        auto playerResource = *memory->playerResource;

        if (!playerResource) return;

        static std::string clanTmp;
        std::string clan = (playerResource->getClan(stealFromIdx));

        if (clanTmp == clan) return;

        memory->setClanTag(clan.c_str(), clan.c_str());
        clanTmp = clan;
        };

    static auto custom = [](bool updateCustom) {

        static std::string clan;
        static std::string clanTemp;
        static std::string addRemoveTemp;
        float realTime = memory->globalVars->realtime;
        static float realTimeSwitcher = 0.f;
        static unsigned int addRemoveMod = 0;

        auto upd = []() {

            if (clanTemp == config->clanCfg.custom.tag) return;

            clanTemp = config->clanCfg.custom.tag;
            clan = config->clanCfg.custom.tag;


            while (clan.find("\\a") != std::string::npos)
                clan.replace(clan.find("\\a"), 2, "\a");

            while (clan.find("\\b") != std::string::npos)
                clan.replace(clan.find("\\b"), 2, "\b");

            while (clan.find("\\f") != std::string::npos)
                clan.replace(clan.find("\\f"), 2, "\f");

            while (clan.find("\\n") != std::string::npos)
                clan.replace(clan.find("\\n"), 2, "\n");

            while (clan.find("\\r") != std::string::npos)
                clan.replace(clan.find("\\r"), 2, "\r");

            while (clan.find("\\t") != std::string::npos)
                clan.replace(clan.find("\\t"), 2, "\t");

            while (clan.find("\\v") != std::string::npos)
                clan.replace(clan.find("\\v"), 2, "\v");

            return;
            };

        upd();

        if (realTime - realTimeSwitcher < config->clanCfg.custom.speed) return;
        addRemoveMod++;

        switch (config->clanCfg.custom.mode) {
        default:
        case 0: // static
            break;
        case 1: // rotate
            if (const auto offset = Helpers::utf8SeqLen(clan[0]); offset <= clan.length())
                std::rotate(clan.begin(), clan.begin() + offset, clan.end());
            break;
        case 2: // rotate backwards
            if (const auto offset = Helpers::utf8SeqLen(clan[0]); offset <= clan.length())
                std::rotate(clan.rbegin(), clan.rbegin() + offset, clan.rend());
            break;
        case 3: // add
            addRemoveTemp = clanTemp;
            clan = addRemoveTemp.substr(0, addRemoveMod % addRemoveTemp.size() + 1);
            break;
        case 4: // remove
            std::string addRemoveTemp;
            addRemoveTemp = clanTemp;
            clan = addRemoveTemp.substr(0, clanTemp.size() - addRemoveMod % addRemoveTemp.size());
            break;
        }

        realTimeSwitcher = realTime;

        memory->setClanTag(std::string(config->clanCfg.custom.prefix).append(clan).append(config->clanCfg.custom.postfix).append(config->clanCfg.custom.hideName ? "\xE2\x80\xA9" : "").c_str(), config->clanCfg.custom.teamTag.c_str());
        };

    static auto string = [](const char* clanText) {
        memory->setClanTag(clanText, "| BLACKHOLE |");
        };
    static auto clock = []() {
        if (config->clanCfg.mode != 2) return;

        static auto lastTime = 0.0f;

        if (memory->globalVars->realtime - lastTime < 1.0f)
            return;

        const auto time = std::time(nullptr);
        const auto localTime = std::localtime(&time);
        char s[11];
        s[0] = '\0';
        snprintf(s, sizeof(s), "[%02d:%02d:%02d]", localTime->tm_hour, localTime->tm_min, localTime->tm_sec);
        lastTime = memory->globalVars->realtime;
        memory->setClanTag(s, "| BLACKHOLE |");
        };
    static auto velocity = []() {
        const auto vel = localPlayer->velocity().length2D();
        std::string velstring = std::to_string((int)vel);
        memory->setClanTag(velstring.c_str(), std::string(velstring).append("km/h WPIERDOL PEEK").c_str());
        };
    static auto position = []() {
        const auto place = localPlayer->lastPlaceName();
        memory->setClanTag(place, "| BLACKHOLE |");
        };
    static auto health = []() {
        const int hp = localPlayer->health();
        std::string result = std::to_string(hp) + "HP";
        memory->setClanTag(result.c_str(), "| BLACKHOLE |");
        };

    if (realTime - timeToUpdate > 1.f) {
        ClanTagStealer::update();
        timeToUpdate = realTime;
    }

    if (realTime - prevTime < 0.15f) return;

    prevTime = realTime;

    switch (config->clanCfg.mode) {
    default: return;

    case 1: BLACKHOLEClanTag(); return;
    case 2: clock(); return;
    case 3: string("\u202e\u202e"); return;
    case 4: velocity(); return;
    case 5: position(); return;
    case 6: health(); return;
    case 7: string("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"); return;
    case 8: gamesenzeClanTag(); return;
    case 9: custom(update); return;
    case 10: steal(); return;
    }

}

//0 - Off
//1 - BLACKHOLE
//2 - Clock
//3 - Reverse
//4 - Velocity
//5 - Last Place
//6 - HP
//7 - \n overflow
//8 - gamesense
//10 - Custom


void Clan::SetStealFromIdx(int idx)
{
    stealFromIdx = idx;
}

void Clan::SetStealEnabled(bool enabled)
{
    stealEnabled = enabled;
}
