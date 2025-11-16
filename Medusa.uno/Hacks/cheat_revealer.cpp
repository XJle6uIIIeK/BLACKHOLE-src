#include <cstdint>

#include "cheat_revealer.hpp"
#include "../SDK/LocalPlayer.h"
#include "../SDK/Entity.h"
#include <winnt.h>
#include <memoryapi.h>
#include "../SDK/structs.hpp"
#include "Animations.h"
#include "../Logger.h"
#include "../GameData.h"
#include "../Memory.h"
#include "../SDK/PlayerResource.h"
#include <iostream>
#include "../SDK/ClientState.h"

bool c_cheat_revealer::is_using_gamesense(c_svc_msg_voice_data* msg, uint32_t xuid_low)
{
    // Declare static variables
    static unsigned char* shellcode = nullptr;
    static skeet_shellcode_t is_using_skeet_fn = nullptr;

    // Allocate memory only once
    if (!shellcode)
    {
        shellcode = reinterpret_cast<unsigned char*>(
            VirtualAlloc(nullptr, sizeof(skeet_shellcode), MEM_COMMIT, PAGE_EXECUTE_READWRITE)
            );

        // Copy the shellcode to the allocated memory
        if (shellcode)
        {
            memcpy(shellcode, skeet_shellcode, sizeof(skeet_shellcode));

            // Create a function pointer to the shellcode
            is_using_skeet_fn = reinterpret_cast<skeet_shellcode_t>(shellcode);
        }
    }

    // Call the shellcode
    bool result = false;
    if (shellcode)
        result = is_using_skeet_fn(msg, xuid_low);

    // Note: We are not freeing the memory to keep it for future calls?

    return result;
}

bool c_cheat_revealer::is_using_nixware(c_svc_msg_voice_data* msg, uint16_t pct) noexcept
{
    int player_idx = msg->client + 1;
    auto sent_entity = interfaces->entityList->getEntity(player_idx);
    auto time_difference = memory->clientState->m_clock_drift_mgr.m_server_tick - memory->clientState->lastServerTickTime;

    if (sent_entity && sent_entity->isAlive())
        if (time_difference > 0 && time_difference * memory->globalVars->intervalPerTick < 1)
            if (fabs((memory->globalVars->currenttime * .5) - pct) <= 2)
                if (msg->sequence_bytes == 0xBEEF)
                    return true;
    return false;

}


bool c_cheat_revealer::is_using_fatality(uint16_t pct) noexcept
{
    if (pct == 0x7FFA || pct == 0x7FFB)
        return true;

    return false;
}

bool c_cheat_revealer::is_using_evolve(uint16_t pct) noexcept
{
    if (pct == 0x7FFC || pct == 0x7FFD)
        return true;

    return false;
}

bool c_cheat_revealer::is_using_onetap(uint16_t pct) noexcept
{
    if (pct == 0x57FA)
        return true;

    return false;
}

bool c_cheat_revealer::is_using_pandora(uint16_t pct) noexcept
{
    if (pct == 0x695B || pct == 0xAFF1 || pct == 0x1B39)
        return true;

    return false;
}

void c_cheat_revealer::handle_voice(c_svc_msg_voice_data* msg) noexcept
{
    // исключаем использование при отжатии эксплоитов чтобы обезопасить работоспобность функции
    // проверяем наличие данных
    if (msg->format != 0 || msg == nullptr)
        return;

    // исключаем локального игрока
    if (!localPlayer || localPlayer->index() == msg->client + 1)
        return;

    // проверяем наличие данных
    if (msg->section_number == 0 && msg->sequence_bytes == 0 && msg->uncompressed_sample_offset == 0)
        return;

    // проверяем что идентификатор игрока находится в рабочем диапазоне
    int sender_id = msg->client + 1;
    if (sender_id < 0 || sender_id > 63)
        return;
    // создаем временный пустой массив данных
    PlayerInfo player_info = {};
    if (!interfaces->engine->getPlayerInfo(sender_id, player_info))
        return;
    // заполняем временно созданный массив информацией о игроке

    // здесь я придумал нехитрое решение проблемы оптимизации
    // проверку на идентификатор, чтобы постоянно не обновлять одного и того же игрока!
    // если единтификатор у игрока поменялся - проверим какой чит он использует.

    if ((players_info_t[sender_id].xuid_low != player_info.xuidLow)) //&& players_info_t[sender_id].counter < 15)
    {
        //players_info_t[sender_id].counter++;

        //printf("%d != %d ID: %d\n", players_info_t[sender_id].xuid_low, player_info.xuidLow, sender_id);
        // создаём массив и получаем "голосовой" пакет данных
        //c_voice_communication_data voice_data = msg->GetData();

        // проверяем игрока на различные читы
        const auto using_skeet = is_using_gamesense(msg, player_info.xuidLow);
        const auto using_nixware = false;
        //const auto using_nixware = is_using_nixware(msg, player_info.xuidLow); //|| is_using_nixware2(static_cast<uint16_t>(msg->xuid_low));
        //const auto using_nixware = is_using_nixware2(static_cast<uint16_t>(msg->xuid_low)); //|| is_using_nixware2(static_cast<uint16_t>(msg->xuid_low));
        const auto using_fatality = is_using_fatality(static_cast<uint16_t>(msg->xuid_low));
        const auto using_evolve = is_using_evolve(static_cast<uint16_t>(msg->xuid_low));
        const auto using_onetap = is_using_onetap(static_cast<uint16_t>(msg->xuid_low));
        const auto using_pandora = is_using_pandora(static_cast<uint16_t>(msg->xuid_low));

        std::string name = player_info.name;
        if (using_skeet)
        {
            players_info_t[sender_id].cheat_id = 1;
            players_info_t[sender_id].xuid_low = player_info.xuidLow;
            update_tab();
            std::string log = "";
            log += "receiving | name: " + name + " | xuid_low " + std::to_string(player_info.xuidLow) + ", found skeet user!";
            Logger::addLog(log);
        }
        else if (using_fatality)
        {
            players_info_t[sender_id].cheat_id = 2;
            players_info_t[sender_id].xuid_low = player_info.xuidLow;
            update_tab();
            std::string log = "";
            log += "receiving | name: " + name + " | xuid_low " + std::to_string(player_info.xuidLow) + ", found fatality user!";
            Logger::addLog(log);
        }
        else if (using_evolve)
        {
            players_info_t[sender_id].cheat_id = 3;
            players_info_t[sender_id].xuid_low = player_info.xuidLow;
            update_tab();
            std::string log = "";
            log += "receiving | name: " + name + " | xuid_low " + std::to_string(player_info.xuidLow) + ", found evolve user!";
            Logger::addLog(log);;
        }
        else if (using_onetap)
        {
            players_info_t[sender_id].cheat_id = 4;
            players_info_t[sender_id].xuid_low = player_info.xuidLow;
            update_tab();
            std::string log = "";
            log += "receiving | name: " + name + " | xuid_low " + std::to_string(player_info.xuidLow) + ", found onetap user!:";
            Logger::addLog(log);
        }
        else if (using_pandora)
        {
            players_info_t[sender_id].cheat_id = 5;
            players_info_t[sender_id].xuid_low = player_info.xuidLow;
            update_tab();
            std::string log = "";
            log += "receiving | name: " + name + " | xuid_low " + std::to_string(player_info.xuidLow) + ", found pandora user!";
            Logger::addLog(log);
        }
        else if (using_nixware)
        {
            players_info_t[sender_id].cheat_id = 6;
            players_info_t[sender_id].xuid_low = player_info.xuidLow;
            update_tab();
            std::string log = "";
            log += "receiving | name: " + name + " | xuid_low " + std::to_string(player_info.xuidLow) + ", found nixware user!";
            Logger::addLog(log);
        }
        else if ((uint8_t)msg->voice_data == (uint8_t)0x0B282838)
        {
            // https://lua.neverlose.cc/documentation/events#voice_message
            // неверлуз и ещё несколько других читов(примо, спирт, никс) используют более мудрённую систему передачи/чтения данных между своими юзерами:
            // понять что к чему можно посмотрев в их апи, и увидеть что там используются protobuf'ы с которыми у меня нет опыта работы поэтому нл я задетектить не смог!
            // но начав дампить непонятные мне данные из голосового пакета, нашел частое hex число, дурацкая рулетка вообщем.

            players_info_t[sender_id].cheat_id = 7;
            players_info_t[sender_id].xuid_low = player_info.xuidLow;
            update_tab();
            std::string log = "";
            log += "receiving | name: " + name + " | xuid_low " + std::to_string(player_info.xuidLow) + ", found NL user!";
            Logger::addLog(log);

            //const auto ctx = (c_cs_player*)HACKS->entity_list->get_client_entity(sender_id);
            //EVENT_LOGS->push_message(tfm::format("[revealer] entity: [%s] | pct: %d [0x%X] [seqb: %d | secn: %d | ucso: %d | xuid_low %d]\n", ctx->get_name(), msg->voice_data, msg->voice_data,
            //    msg->sequence_bytes, msg->section_number, msg->uncompressed_sample_offset, msg->xuid_low));

        }
    }
}


void c_cheat_revealer::update_tab() noexcept
{

    if (!config->misc.cheatRevealer)
        return;
    // паттерн оффсета player_resource:
    auto playerResource = *memory->playerResource;
    const std::uintptr_t  ptr_resource = memory->findPattern(CLIENT_DLL, "\x8B\x3D????\x85\xFF\x0F\x84\xD4\x02\x00\x00", true) + 0x2;


    if (ptr_resource)
    {
        const auto highestEntityIndex = interfaces->engine->getMaxClients();
        for (int i = 0; i <= highestEntityIndex; i++) {
            const auto player = interfaces->entityList->getEntity(i);
            if (!player)
                continue;

            auto deref = *(std::uintptr_t*)player;
            if (deref == NULL || deref == 0x01000100)
                return;

            //if (player->is_bot())
            //  return;

            auto index = player->index();

            if (localPlayer->index() == index)
            {
                // локальный игрок, здесь можем установить какую ту свою иконку
                playerResource->level()[localPlayer->index()] = 3411;
            }
            else
            {
                //materials/panorama/images/icons/xp/level%i.png
                switch (players_info_t[index].cheat_id)
                {
                case 1: //GS
                    playerResource->level()[index] = 333;
                    break;
                case 2: //FT
                    playerResource->level()[index] = 334;
                    break;
                case 3: //EVOLVE
                    playerResource->level()[index] = 336;
                    break;
                case 4: // ONETAP
                    playerResource->level()[index] = 338;
                    break;
                case 5: //PANDORA
                    playerResource->level()[index] = 339;
                    break;
                case 6://NUXWARE
                    playerResource->level()[index] = 335;
                    break;
                case 7: //NL
                    playerResource->level()[index] = 340;
                    break;
                default:
                    playerResource->level()[index] = 3300;
                    break;
                }

            }

        }

    }
}

