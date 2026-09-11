/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RandomPlayerbotFactory.h"
#include "AccountMgr.h"
#include "ArenaTeamMgr.h"
#include "CharacterCache.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RaceMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SocialMgr.h"
#include "Timer.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <unordered_map>

namespace
{
    struct RandomBotConfBlacklist
    {
        std::set<uint16> raceClassPairs;
        std::unordered_map<uint16, std::set<uint8>> faces;
        std::unordered_map<uint16, std::set<uint8>> skins;
        std::unordered_map<uint16, std::set<uint8>> hairStyles;
        bool loaded = false;
    };

    RandomBotConfBlacklist sRandomBotConfBlacklist;

    std::string Trim(std::string value)
    {
        auto const isNotSpace = [](unsigned char c) { return !std::isspace(c); };

        value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(), value.end());
        return value;
    }

    std::string NormalizeName(std::string value)
    {
        std::string normalized;
        normalized.reserve(value.size());

        for (unsigned char c : value)
        {
            if (std::isspace(c) || c == '_' || c == '-')
                continue;

            normalized.push_back(static_cast<char>(std::tolower(c)));
        }

        return normalized;
    }

    bool TryParseRaceName(std::string const& value, uint8& race)
    {
        std::string const name = NormalizeName(value);

        if (name == "human")     { race = RACE_HUMAN; return true; }
        if (name == "orc")       { race = RACE_ORC; return true; }
        if (name == "dwarf")     { race = RACE_DWARF; return true; }
        if (name == "nightelf")  { race = RACE_NIGHTELF; return true; }
        if (name == "undead" || name == "scourge")
                                 { race = RACE_UNDEAD_PLAYER; return true; }
        if (name == "tauren")    { race = RACE_TAUREN; return true; }
        if (name == "gnome")     { race = RACE_GNOME; return true; }
        if (name == "troll")     { race = RACE_TROLL; return true; }
        if (name == "bloodelf")  { race = RACE_BLOODELF; return true; }
        if (name == "draenei")   { race = RACE_DRAENEI; return true; }

        return false;
    }

    bool TryParseClassName(std::string const& value, uint8& cls)
    {
        std::string const name = NormalizeName(value);

        if (name == "warrior")       { cls = CLASS_WARRIOR; return true; }
        if (name == "paladin")       { cls = CLASS_PALADIN; return true; }
        if (name == "hunter")        { cls = CLASS_HUNTER; return true; }
        if (name == "rogue")         { cls = CLASS_ROGUE; return true; }
        if (name == "priest")        { cls = CLASS_PRIEST; return true; }
        if (name == "deathknight" || name == "dk")
                                     { cls = CLASS_DEATH_KNIGHT; return true; }
        if (name == "shaman")        { cls = CLASS_SHAMAN; return true; }
        if (name == "mage")          { cls = CLASS_MAGE; return true; }
        if (name == "warlock")       { cls = CLASS_WARLOCK; return true; }
        if (name == "druid")         { cls = CLASS_DRUID; return true; }

        return false;
    }

    uint16 MakeRaceClassKey(uint8 race, uint8 cls)
    {
        return (static_cast<uint16>(race) << 8) | static_cast<uint16>(cls);
    }

    uint16 MakeRaceGenderKey(uint8 race, uint8 gender)
    {
        return (static_cast<uint16>(race) << 8) | static_cast<uint16>(gender);
    }

    bool TryParseByte(std::string const& value, uint8& result)
    {
        std::string const trimmed = Trim(value);
        if (trimmed.empty())
            return false;

        uint32 parsed = 0;
        for (unsigned char c : trimmed)
        {
            if (c < '0' || c > '9')
                return false;

            parsed = parsed * 10 + static_cast<uint32>(c - '0');
            if (parsed > 255)
                return false;
        }

        result = static_cast<uint8>(parsed);
        return true;
    }

    void ParseRaceClassBlacklist(std::string const& configValue)
    {
        std::size_t start = 0;

        while (start <= configValue.size())
        {
            std::size_t const comma = configValue.find(',', start);
            std::string entry = Trim(configValue.substr(
                start,
                comma == std::string::npos ? std::string::npos : comma - start));

            if (!entry.empty())
            {
                std::size_t const colon = entry.find(':');
                if (colon == std::string::npos)
                {
                    LOG_WARN("playerbots",
                        "Ignoring invalid AiPlayerbot.RandomBotBlacklist.RaceClass entry '{}'. "
                        "Expected race:class.", entry);
                }
                else
                {
                    uint8 race = 0;
                    uint8 cls = 0;
                    std::string const raceText = Trim(entry.substr(0, colon));
                    std::string const classText = Trim(entry.substr(colon + 1));

                    if (!TryParseRaceName(raceText, race))
                    {
                        LOG_WARN("playerbots",
                            "Ignoring RandomBot blacklist entry '{}': unknown race '{}'.",
                            entry, raceText);
                    }
                    else if (!TryParseClassName(classText, cls))
                    {
                        LOG_WARN("playerbots",
                            "Ignoring RandomBot blacklist entry '{}': unknown class '{}'. "
                            "Example spelling: paladin.", entry, classText);
                    }
                    else
                    {
                        sRandomBotConfBlacklist.raceClassPairs.insert(MakeRaceClassKey(race, cls));
                    }
                }
            }

            if (comma == std::string::npos)
                break;

            start = comma + 1;
        }
    }

    void ParseFaceBlacklist(std::string const& configKey, uint8 race, uint8 gender)
    {
        // showLogs=false is intentional: users only need to define the race/gender
        // lines they actually want, without the server warning about every omitted key.
        std::string const configValue =
            sConfigMgr->GetOption<std::string>(configKey, "", false);

        if (configValue.empty())
            return;

        std::set<uint8>& bannedFaces =
            sRandomBotConfBlacklist.faces[MakeRaceGenderKey(race, gender)];

        std::size_t start = 0;
        while (start <= configValue.size())
        {
            std::size_t const comma = configValue.find(',', start);
            std::string const entry = configValue.substr(
                start,
                comma == std::string::npos ? std::string::npos : comma - start);

            if (!Trim(entry).empty())
            {
                uint8 face = 0;
                if (!TryParseByte(entry, face))
                {
                    LOG_WARN("playerbots",
                        "Ignoring invalid Face value '{}' in {}. Expected 0-255.",
                        Trim(entry), configKey);
                }
                else
                {
                    bannedFaces.insert(face);
                }
            }

            if (comma == std::string::npos)
                break;

            start = comma + 1;
        }
    }

    void ParseSkinBlacklist(std::string const& configKey, uint8 race, uint8 gender)
    {
        // showLogs=false is intentional: users only need to define the race/gender
        // lines they actually want, without the server warning about every omitted key.
        std::string const configValue =
            sConfigMgr->GetOption<std::string>(configKey, "", false);

        if (configValue.empty())
            return;

        std::set<uint8>& bannedSkins =
            sRandomBotConfBlacklist.skins[MakeRaceGenderKey(race, gender)];

        std::size_t start = 0;
        while (start <= configValue.size())
        {
            std::size_t const comma = configValue.find(',', start);
            std::string const entry = configValue.substr(
                start,
                comma == std::string::npos ? std::string::npos : comma - start);

            if (!Trim(entry).empty())
            {
                uint8 skin = 0;
                if (!TryParseByte(entry, skin))
                {
                    LOG_WARN("playerbots",
                        "Ignoring invalid Skin value '{}' in {}. Expected 0-255.",
                        Trim(entry), configKey);
                }
                else
                {
                    bannedSkins.insert(skin);
                }
            }

            if (comma == std::string::npos)
                break;

            start = comma + 1;
        }
    }

    void ParseHairStyleBlacklist(std::string const& configKey, uint8 race, uint8 gender)
    {
        // showLogs=false is intentional: users only need to define the race/gender
        // lines they actually want, without the server warning about every omitted key.
        std::string const configValue =
            sConfigMgr->GetOption<std::string>(configKey, "", false);

        if (configValue.empty())
            return;

        std::set<uint8>& bannedHairStyles =
            sRandomBotConfBlacklist.hairStyles[MakeRaceGenderKey(race, gender)];

        std::size_t start = 0;
        while (start <= configValue.size())
        {
            std::size_t const comma = configValue.find(',', start);
            std::string const entry = configValue.substr(
                start,
                comma == std::string::npos ? std::string::npos : comma - start);

            if (!Trim(entry).empty())
            {
                uint8 hairStyle = 0;
                if (!TryParseByte(entry, hairStyle))
                {
                    LOG_WARN("playerbots",
                        "Ignoring invalid HairStyle value '{}' in {}. Expected 0-255.",
                        Trim(entry), configKey);
                }
                else
                {
                    bannedHairStyles.insert(hairStyle);
                }
            }

            if (comma == std::string::npos)
                break;

            start = comma + 1;
        }
    }

    void LoadRandomBotConfBlacklist(bool forceReload = false)
    {
        if (sRandomBotConfBlacklist.loaded && !forceReload)
            return;

        sRandomBotConfBlacklist.loaded = true;
        sRandomBotConfBlacklist.raceClassPairs.clear();
        sRandomBotConfBlacklist.faces.clear();
        sRandomBotConfBlacklist.skins.clear();
        sRandomBotConfBlacklist.hairStyles.clear();

        std::string const raceClassValue =
            sConfigMgr->GetOption<std::string>(
                "AiPlayerbot.RandomBotBlacklist.RaceClass", "", false);

        ParseRaceClassBlacklist(raceClassValue);

        struct RaceConfigName
        {
            char const* name;
            uint8 race;
        };

        RaceConfigName const races[] =
        {
            {"Human", RACE_HUMAN},
            {"Orc", RACE_ORC},
            {"Dwarf", RACE_DWARF},
            {"NightElf", RACE_NIGHTELF},
            {"Undead", RACE_UNDEAD_PLAYER},
            {"Tauren", RACE_TAUREN},
            {"Gnome", RACE_GNOME},
            {"Troll", RACE_TROLL},
            {"BloodElf", RACE_BLOODELF},
            {"Draenei", RACE_DRAENEI}
        };

        for (RaceConfigName const& race : races)
        {
            ParseFaceBlacklist(
                "AiPlayerbot.RandomBotBlacklist." + std::string(race.name) + ".Male.Face",
                race.race, GENDER_MALE);

            ParseFaceBlacklist(
                "AiPlayerbot.RandomBotBlacklist." + std::string(race.name) + ".Female.Face",
                race.race, GENDER_FEMALE);

            ParseSkinBlacklist(
                "AiPlayerbot.RandomBotBlacklist." + std::string(race.name) + ".Male.Skin",
                race.race, GENDER_MALE);

            ParseSkinBlacklist(
                "AiPlayerbot.RandomBotBlacklist." + std::string(race.name) + ".Female.Skin",
                race.race, GENDER_FEMALE);

            ParseHairStyleBlacklist(
                "AiPlayerbot.RandomBotBlacklist." + std::string(race.name) + ".Male.HairStyle",
                race.race, GENDER_MALE);

            ParseHairStyleBlacklist(
                "AiPlayerbot.RandomBotBlacklist." + std::string(race.name) + ".Female.HairStyle",
                race.race, GENDER_FEMALE);
        }

        uint32 faceValueCount = 0;
        for (auto const& entry : sRandomBotConfBlacklist.faces)
            faceValueCount += static_cast<uint32>(entry.second.size());

        uint32 skinValueCount = 0;
        for (auto const& entry : sRandomBotConfBlacklist.skins)
            skinValueCount += static_cast<uint32>(entry.second.size());

        uint32 hairStyleValueCount = 0;
        for (auto const& entry : sRandomBotConfBlacklist.hairStyles)
            hairStyleValueCount += static_cast<uint32>(entry.second.size());

        LOG_INFO("playerbots",
            "Loaded RandomBot conf blacklist: {} race/class combinations, {} face values, {} skin values, {} hair style values.",
            sRandomBotConfBlacklist.raceClassPairs.size(), faceValueCount, skinValueCount, hairStyleValueCount);
    }

    bool IsRaceClassBlacklisted(uint8 race, uint8 cls)
    {
        LoadRandomBotConfBlacklist();
        return sRandomBotConfBlacklist.raceClassPairs.find(
            MakeRaceClassKey(race, cls)) != sRandomBotConfBlacklist.raceClassPairs.end();
    }

    bool IsFaceBlacklisted(uint8 race, uint8 gender, uint8 face)
    {
        LoadRandomBotConfBlacklist();

        auto const itr = sRandomBotConfBlacklist.faces.find(
            MakeRaceGenderKey(race, gender));

        if (itr == sRandomBotConfBlacklist.faces.end())
            return false;

        return itr->second.find(face) != itr->second.end();
    }

    bool IsSkinBlacklisted(uint8 race, uint8 gender, uint8 skin)
    {
        LoadRandomBotConfBlacklist();

        auto const itr = sRandomBotConfBlacklist.skins.find(
            MakeRaceGenderKey(race, gender));

        if (itr == sRandomBotConfBlacklist.skins.end())
            return false;

        return itr->second.find(skin) != itr->second.end();
    }

    bool IsHairStyleBlacklisted(uint8 race, uint8 gender, uint8 hairStyle)
    {
        LoadRandomBotConfBlacklist();

        auto const itr = sRandomBotConfBlacklist.hairStyles.find(
            MakeRaceGenderKey(race, gender));

        if (itr == sRandomBotConfBlacklist.hairStyles.end())
            return false;

        return itr->second.find(hairStyle) != itr->second.end();
    }

    bool HasAllowedFace(uint8 race, uint8 gender)
    {
        for (CharSectionsEntry const* charSection : sCharSectionsStore)
        {
            if (!charSection ||
                charSection->Race != race ||
                charSection->Gender != gender ||
                charSection->GenType != SECTION_TYPE_FACE)
            {
                continue;
            }

            if (!IsFaceBlacklisted(race, gender, charSection->Type) &&
                !IsSkinBlacklisted(race, gender, charSection->Color))
            {
                return true;
            }
        }

        return false;
    }

    bool HasAllowedHair(uint8 race, uint8 gender)
    {
        for (CharSectionsEntry const* charSection : sCharSectionsStore)
        {
            if (!charSection ||
                charSection->Race != race ||
                charSection->Gender != gender ||
                charSection->GenType != SECTION_TYPE_HAIR)
            {
                continue;
            }

            if (!IsHairStyleBlacklisted(race, gender, charSection->Type))
                return true;
        }

        return false;
    }

    bool HasAnyAllowedRaceForClass(uint8 cls)
    {
        for (uint8 race = RACE_HUMAN; race < sRaceMgr->GetMaxRaces(); ++race)
        {
            if ((1 << (race - 1)) &
                sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_RACEMASK))
            {
                continue;
            }

            uint32 const expansion = sWorld->getIntConfig(CONFIG_EXPANSION);

            if (expansion < EXPANSION_THE_BURNING_CRUSADE &&
                (race == RACE_BLOODELF || race == RACE_DRAENEI))
            {
                continue;
            }

            if (expansion < EXPANSION_WRATH_OF_THE_LICH_KING &&
                cls == CLASS_DEATH_KNIGHT)
            {
                continue;
            }

            if (!sObjectMgr->GetPlayerInfo(race, cls))
                continue;

            if (IsRaceClassBlacklisted(race, cls))
                continue;

            if ((HasAllowedFace(race, GENDER_MALE) && HasAllowedHair(race, GENDER_MALE)) ||
                (HasAllowedFace(race, GENDER_FEMALE) && HasAllowedHair(race, GENDER_FEMALE)))
            {
                return true;
            }
        }

        return false;
    }
}

constexpr RandomPlayerbotFactory::NameRaceAndGender RandomPlayerbotFactory::CombineRaceAndGender(uint8 race,
                                                                                                uint8 gender)
{
    NameRaceAndGender baseIndex;
    switch (race)
    {
        case RACE_ORC:        baseIndex = NameRaceAndGender::OrcMale; break;
        case RACE_DWARF:      baseIndex = NameRaceAndGender::DwarfMale; break;
        case RACE_NIGHTELF:   baseIndex = NameRaceAndGender::NightelfMale; break;
        case RACE_TAUREN:     baseIndex = NameRaceAndGender::TaurenMale; break;
        case RACE_GNOME:      baseIndex = NameRaceAndGender::GnomeMale; break;
        case RACE_TROLL:      baseIndex = NameRaceAndGender::TrollMale; break;
        case RACE_BLOODELF:   baseIndex = NameRaceAndGender::BloodelfMale; break;
        case RACE_DRAENEI:    baseIndex = NameRaceAndGender::DraeneiMale; break;
        case RACE_HUMAN:
        case RACE_UNDEAD_PLAYER:
        default:
            baseIndex = NameRaceAndGender::GenericMale;
            break;
    }

    return static_cast<NameRaceAndGender>(static_cast<uint8>(baseIndex) +
                                          ((gender >= GENDER_NONE) ? static_cast<uint8>(GENDER_MALE) : gender));
}

bool RandomPlayerbotFactory::IsValidRaceClassCombination(uint8 race, uint8 cls, uint32 expansion)
{
    // skip expansion races if not playing with expansion
    if (expansion < EXPANSION_THE_BURNING_CRUSADE && (race == RACE_BLOODELF || race == RACE_DRAENEI))
        return false;

    // skip expansion classes if not playing with expansion
    if (expansion < EXPANSION_WRATH_OF_THE_LICH_KING && cls == CLASS_DEATH_KNIGHT)
        return false;

    if (IsRaceClassBlacklisted(race, cls))
        return false;

    PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
    return info != nullptr;
}

Player* RandomPlayerbotFactory::CreateRandomBot(WorldSession* session, uint8 cls, std::unordered_map<NameRaceAndGender, std::vector<std::string>>& nameCache)
{
    LOG_DEBUG("playerbots", "Creating a new random bot for class: {}", cls);

    struct RaceCandidate
    {
        uint8 race = 0;
        bool maleAvailable = false;
        bool femaleAvailable = false;
    };

    std::vector<RaceCandidate> allianceOptions;
    std::vector<RaceCandidate> hordeOptions;

    for (uint8 race = RACE_HUMAN; race < sRaceMgr->GetMaxRaces(); ++race)
    {
        if ((1 << (race - 1)) &
            sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_RACEMASK))
        {
            continue;
        }

        if (!IsValidRaceClassCombination(
                race, cls, sWorld->getIntConfig(CONFIG_EXPANSION)))
        {
            continue;
        }

        if (IsRaceClassBlacklisted(race, cls))
            continue;

        bool const maleAvailable =
            HasAllowedFace(race, GENDER_MALE) && HasAllowedHair(race, GENDER_MALE);
        bool const femaleAvailable =
            HasAllowedFace(race, GENDER_FEMALE) && HasAllowedHair(race, GENDER_FEMALE);

        if (!maleAvailable && !femaleAvailable)
            continue;

        RaceCandidate const candidate{race, maleAvailable, femaleAvailable};

        if (IsAlliance(race))
            allianceOptions.push_back(candidate);
        else
            hordeOptions.push_back(candidate);
    }

    if (allianceOptions.empty() && hordeOptions.empty())
    {
        LOG_ERROR("playerbots",
            "No races/faces are available for class {} after applying RandomBotBlacklist.",
            cls);
        return nullptr;
    }

    // Preserve the factory's intended 50/50 faction selection whenever both
    // factions still have at least one legal candidate.
    std::vector<RaceCandidate> const* raceOptions = nullptr;

    if (!allianceOptions.empty() && !hordeOptions.empty())
        raceOptions = urand(0, 1) ? &allianceOptions : &hordeOptions;
    else if (!allianceOptions.empty())
        raceOptions = &allianceOptions;
    else
        raceOptions = &hordeOptions;

    RaceCandidate const& candidate =
        (*raceOptions)[urand(0, static_cast<uint32>(raceOptions->size() - 1))];

    uint8 const race = candidate.race;
    uint8 gender = GENDER_MALE;

    if (candidate.maleAvailable && candidate.femaleAvailable)
        gender = urand(0, 1) ? GENDER_MALE : GENDER_FEMALE;
    else if (candidate.femaleAvailable)
        gender = GENDER_FEMALE;

    const auto raceAndGender = CombineRaceAndGender(race, gender);

    std::string name;
    if (!nameCache.empty())
    {
        if (nameCache[raceAndGender].empty())
        {
            LOG_ERROR("playerbots", "No names found for the specified race: {} and gender: {}",
                    race, gender);
            return nullptr;
        }

        uint32 i = urand(0, nameCache[raceAndGender].size() - 1);
        name = nameCache[raceAndGender][i];
        swap(nameCache[raceAndGender][i], nameCache[raceAndGender].back());
        nameCache[raceAndGender].pop_back();
    }
    else
    {
        name = CreateRandomBotName(raceAndGender);
    }

    if (name.empty())
    {
        LOG_ERROR("playerbots", "Failed to get a valid random bot name");
        return nullptr;
    }

    std::vector<uint8> skinColors, facialHairTypes;
    std::vector<std::pair<uint8, uint8>> faces, hairs;

    for (CharSectionsEntry const* charSection : sCharSectionsStore)
    {
        if (!charSection ||
            charSection->Race != race ||
            charSection->Gender != gender)
        {
            continue;
        }

        switch (charSection->GenType)
        {
            case SECTION_TYPE_SKIN:
                skinColors.push_back(charSection->Color);
                break;

            case SECTION_TYPE_FACE:
                // Face.Type is the Face number shown in LB_OnlineRoster.
                // Face.Color continues to supply Skin exactly as in the existing factory.
                if (!IsFaceBlacklisted(race, gender, charSection->Type) &&
                    !IsSkinBlacklisted(race, gender, charSection->Color))
                {
                    faces.push_back(
                        std::pair<uint8, uint8>(
                            charSection->Type,
                            charSection->Color));
                }
                break;

            case SECTION_TYPE_FACIAL_HAIR:
                facialHairTypes.push_back(charSection->Type);
                break;

            case SECTION_TYPE_HAIR:
                if (!IsHairStyleBlacklisted(race, gender, charSection->Type))
                {
                    hairs.push_back(
                        std::pair<uint8, uint8>(
                            charSection->Type,
                            charSection->Color));
                }
                break;
        }
    }

    if (faces.empty())
    {
        LOG_ERROR("playerbots",
            "All Face/Skin combinations are blacklisted or unavailable for race {} gender {}.",
            race, gender);
        return nullptr;
    }

    if (hairs.empty())
    {
        LOG_ERROR("playerbots",
            "All HairStyle values are blacklisted or unavailable for race {} gender {}.",
            race, gender);
        return nullptr;
    }

    std::pair<uint8, uint8> face =
        faces[urand(0, static_cast<uint32>(faces.size() - 1))];

    std::pair<uint8, uint8> hair =
        hairs[urand(0, static_cast<uint32>(hairs.size() - 1))];

    bool const excludeCheck =
        race == RACE_TAUREN ||
        race == RACE_DRAENEI ||
        (gender == GENDER_FEMALE &&
         race != RACE_NIGHTELF &&
         race != RACE_UNDEAD_PLAYER);

    uint8 facialHair = 0;
    if (!excludeCheck)
    {
        if (facialHairTypes.empty())
        {
            LOG_ERROR("playerbots",
                "No facial-style candidates are available for race {} gender {}.",
                race, gender);
            return nullptr;
        }

        facialHair =
            facialHairTypes[
                urand(0, static_cast<uint32>(facialHairTypes.size() - 1))];
    }

    std::unique_ptr<CharacterCreateInfo> characterInfo =
        std::make_unique<CharacterCreateInfo>(
            name,
            race,
            cls,
            gender,
            face.second,
            face.first,
            hair.first,
            hair.second,
            facialHair);

    Player* player = new Player(session);
    player->GetMotionMaster()->Initialize();

    if (!player->Create(
            sObjectMgr->GetGenerator<HighGuid::Player>().Generate(),
            characterInfo.get()))
    {
        player->CleanupsBeforeDelete();
        delete player;

        LOG_ERROR("playerbots",
            "Unable to create random bot - name: \"{}\", race: {}, class: {}, gender: {}, face: {}",
            name.c_str(), race, cls, gender, face.first);
        return nullptr;
    }

    player->setCinematic(2);
    player->SetAtLoginFlag(AT_LOGIN_NONE);

    if (cls == CLASS_DEATH_KNIGHT)
        player->learnSpell(50977, false);

    LOG_DEBUG("playerbots",
        "Random bot created - name: \"{}\", race: {}, class: {}, gender: {}, face: {}",
        name.c_str(), race, cls, gender, face.first);

    return player;
}

std::string const RandomPlayerbotFactory::CreateRandomBotName(NameRaceAndGender raceAndGender)
{
    std::string botName = "";
    int tries = 3;
    while (--tries)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT n.name "
            "FROM playerbots_names n "
            "LEFT OUTER JOIN characters c ON c.name = n.name "
            "WHERE c.guid IS NULL and n.gender = '{}' "
            "ORDER BY RAND() LIMIT 1",
            static_cast<uint8>(raceAndGender));
        if (!result)
        {
            break;
        }

        Field* fields = result->Fetch();
        botName = fields[0].Get<std::string>();
        if (ObjectMgr::CheckPlayerName(botName) == CHAR_NAME_SUCCESS)  // Checks for reservation & profanity, too
        {
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
            stmt->SetData(0, botName);

            if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
                continue;

            return botName;
        }
    }

    // CONLANG NAME GENERATION
    LOG_ERROR("playerbots", "No more names left for random bots. Attempting conlang name generation.");
    const std::string groupCategory = "SCVKRU";
    const std::string groupFormStart[2][4] = {{"SV", "SV", "VK", "RV"}, {"V", "SU", "VS", "RV"}};
    const std::string groupFormMid[2][6] = {{"CV", "CVC", "CVC", "CVK", "VC", "VK"},
                                            {"CV", "CVC", "CVK", "KVC", "VC", "KV"}};
    const std::string groupFormEnd[2][4] = {{"CV", "VC", "VK", "CV"}, {"RU", "UR", "VR", "V"}};
    const std::string groupLetter[2][6] = {
        // S           C                            V               K           R         U
        {"dtspkThfS", "bcCdfghjkmnNqqrrlsStTvwxyz", "aaeeiouA", "ppttkkbdg", "lmmnrr", "AEO"},
        {"dtskThfS", "bcCdfghjkmmnNqrrlssStTvwyz", "aaaeeiiuAAEIO", "ppttkbbdg", "lmmnrrr", "AEOy"}};
    const std::string replaceRule[2][17] = {
        {"ST", "ka", "ko", "ku", "kr", "S", "T", "C", "N", "jj", "AA", "AI", "A", "E", "O", "I", "aa"},
        {"sth", "ca", "co", "cu", "cr", "sh", "th", "ch", "ng", "dg", "A", "ayu", "ai", "ei", "ou", "iu", "ae"}};

    const auto gender = static_cast<uint8>(raceAndGender) % 2;

    tries = 10;
    while (--tries)
    {
        botName.clear();
        // Build name from groupForms
        // Pick random start group
        botName = groupFormStart[gender][rand() % 4];
        // Pick up to 2 and then up to 1 additional middle group
        for (int i = 0; i < rand() % 3 + rand() % 2; i++)
        {
            botName += groupFormMid[gender][rand() % 6];
        }
        // Pick up to 1 end group
        botName += rand() % 2 ? groupFormEnd[gender][rand() % 4] : "";
        // If name is single letter add random end group
        botName += (botName.size() < 2) ? groupFormEnd[gender][rand() % 4] : "";

        // Replace Catagory value with random Letter from that Catagory's Letter string for a given bot gender
        for (size_t i = 0; i < botName.size(); i++)
        {
            botName[i] = groupLetter[gender][groupCategory.find(botName[i])]
                                    [rand() % groupLetter[gender][groupCategory.find(botName[i])].size()];
        }

        // Itterate over replace rules
        for (int i = 0; i < 17; i++)
        {
            int j = botName.find(replaceRule[0][i]);
            while (j > -1)
            {
                botName.replace(j, replaceRule[0][i].size(), replaceRule[1][i]);
                j = botName.find(replaceRule[0][i]);
            }
        }

        // Capitalize first letter
        botName[0] -= 32;

        if (ObjectMgr::CheckPlayerName(botName) != CHAR_NAME_SUCCESS) // Checks for reservation & profanity, too
        {
            botName.clear();
            continue;
        }
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
        stmt->SetData(0, botName);

        if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        {
            botName.clear();
            continue;
        }
        return botName;
    }

    // TRUE RANDOM NAME GENERATION
    LOG_ERROR("playerbots", "Con​lang name generation failed. True random name fallback.");
    tries = 10;
    while (--tries)
    {
        for (uint8 i = 0; i < 10; i++)
        {
            botName += (i == 0 ? 'A' : 'a') + rand() % 26;
        }
        if (ObjectMgr::CheckPlayerName(botName) != CHAR_NAME_SUCCESS)  // Checks for reservation & profanity, too
        {
            botName.clear();
            continue;
        }
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
        stmt->SetData(0, botName);

        if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        {
            botName.clear();
            continue;
        }
        return botName;
    }
    LOG_ERROR("playerbots", "Random name generation failed.");
    botName.clear();
    return botName;
}

// Calculates the total number of required accounts, either using the specified randomBotAccountCount
// or determining it dynamically based on MaxRandomBots, EnablePeriodicOnlineOffline and its ratio,
// and AddClassAccountPoolSize. The system also factors in the types of existing account, as assigned by
// AssignAccountTypes()
uint32 RandomPlayerbotFactory::CalculateTotalAccountCount()
{
    // Reset account types if features are disabled
    // Reset is done here to precede needed accounts calculations
    if (sPlayerbotAIConfig.maxRandomBots == 0 || sPlayerbotAIConfig.addClassAccountPoolSize == 0)
    {
        if (sPlayerbotAIConfig.maxRandomBots == 0)
        {
            PlayerbotsDatabase.Execute("UPDATE playerbots_account_type SET account_type = 0 WHERE account_type = 1");
            LOG_INFO("playerbots", "MaxRandomBots set to 0, any RNDbot accounts (type 1) will be unassigned (type 0)");
        }
        if (sPlayerbotAIConfig.addClassAccountPoolSize == 0)
        {
            PlayerbotsDatabase.Execute("UPDATE playerbots_account_type SET account_type = 0 WHERE account_type = 2");
            LOG_INFO("playerbots", "AddClassAccountPoolSize set to 0, any AddClass accounts (type 2) will be unassigned (type 0)");
        }

        // Wait for DB to reflect the change, up to 1 second max. This is needed to make sure other logs don't show wrong info
        for (int waited = 0; waited < 1000; waited += 50)
        {
            QueryResult res = PlayerbotsDatabase.Query("SELECT COUNT(*) FROM playerbots_account_type WHERE account_type IN ({}, {})",
                sPlayerbotAIConfig.maxRandomBots == 0 ? 1 : -1,
                sPlayerbotAIConfig.addClassAccountPoolSize == 0 ? 2 : -1);

            if (!res || res->Fetch()[0].Get<uint64>() == 0)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(50));     // Extra 50ms fixed delay for safety.
        }
    }

    // Check existing account types
    uint32 existingRndBotAccounts = 0;
    uint32 existingAddClassAccounts = 0;
    uint32 existingUnassignedAccounts = 0;

    QueryResult typeCheck = PlayerbotsDatabase.Query("SELECT account_type, COUNT(*) FROM playerbots_account_type GROUP BY account_type");
    if (typeCheck)
    {
        do
        {
            Field* fields = typeCheck->Fetch();
            uint8 accountType = fields[0].Get<uint8>();
            uint32 count = static_cast<uint32>(fields[1].Get<uint64>());

            if (accountType == 0) existingUnassignedAccounts = count;
            else if (accountType == 1) existingRndBotAccounts = count;
            else if (accountType == 2) existingAddClassAccounts = count;
        } while (typeCheck->NextRow());
    }

    // Determine divisor based on Death Knight availability and requested A&H faction ratio
    int divisor = CalculateAvailableCharsPerAccount();

    // Calculate max bots
    int maxBots = sPlayerbotAIConfig.maxRandomBots;
    // Take periodic online/offline into account
    if (sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        maxBots *= sPlayerbotAIConfig.periodicOnlineOfflineRatio;

    // Calculate number of accounts needed for RNDbots
    // Result is rounded up for maxBots not cleanly divisible by the divisor
    uint32 neededRndBotAccounts = (maxBots + divisor - 1) / divisor;
    uint32 neededAddClassAccounts = sPlayerbotAIConfig.addClassAccountPoolSize;

    // Start with existing total
    uint32 existingTotal = existingRndBotAccounts + existingAddClassAccounts + existingUnassignedAccounts;

    // Calculate shortfalls after using unassigned accounts
    uint32 availableUnassigned = existingUnassignedAccounts;
    uint32 additionalAccountsNeeded = 0;

    // Check RNDbot needs
    if (neededRndBotAccounts > existingRndBotAccounts)
    {
        uint32 rndBotShortfall = neededRndBotAccounts - existingRndBotAccounts;
        if (rndBotShortfall <= availableUnassigned)
            availableUnassigned -= rndBotShortfall;
        else
        {
            additionalAccountsNeeded += (rndBotShortfall - availableUnassigned);
            availableUnassigned = 0;
        }
    }

    // Check AddClass needs
    if (neededAddClassAccounts > existingAddClassAccounts)
    {
        uint32 addClassShortfall = neededAddClassAccounts - existingAddClassAccounts;
        if (addClassShortfall <= availableUnassigned)
            availableUnassigned -= addClassShortfall;
        else
        {
            additionalAccountsNeeded += (addClassShortfall - availableUnassigned);
            availableUnassigned = 0;
        }
    }

    // Return existing total plus any additional accounts needed
    uint32 calculatedTotal = existingTotal + additionalAccountsNeeded;

    // Manually set randomBotAccountCount meets the requirements
    if (sPlayerbotAIConfig.randomBotAccountCount >= calculatedTotal)
        return sPlayerbotAIConfig.randomBotAccountCount;
    // Manually set randomBotAccountCount doesn't meet the requirements. Using calculated value
    if (sPlayerbotAIConfig.randomBotAccountCount > 0)
        LOG_WARN("playerbots", "RandomBotAccountCount ({}) is lower than the required calculated value ({}). Using the calculated value instead.",
            sPlayerbotAIConfig.randomBotAccountCount, calculatedTotal);

    return calculatedTotal;
}

uint32 RandomPlayerbotFactory::CalculateAvailableCharsPerAccount()
{
    LoadRandomBotConfBlacklist();

    uint32 availableChars = 0;

    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        if (!((1 << (cls - 1)) & CLASSMASK_ALL_PLAYABLE) ||
            !sChrClassesStore.LookupEntry(cls))
        {
            continue;
        }

        if ((1 << (cls - 1)) &
            sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK))
        {
            continue;
        }

        if (sPlayerbotAIConfig.disableDeathKnightLogin &&
            cls == CLASS_DEATH_KNIGHT)
        {
            continue;
        }

        if (HasAnyAllowedRaceForClass(cls))
            ++availableChars;
    }

    if (availableChars == 0)
    {
        LOG_ERROR("playerbots",
            "RandomBotBlacklist leaves no creatable classes. "
            "Using divisor 1 to avoid division by zero.");
        return 1;
    }

    uint32 const hordeRatio = sPlayerbotAIConfig.randomBotHordeRatio;
    uint32 const allianceRatio = sPlayerbotAIConfig.randomBotAllianceRatio;
    uint32 const largestRatio = std::max(hordeRatio, allianceRatio);

    if (largestRatio > 0)
    {
        float const unavailableRatio =
            static_cast<float>(
                largestRatio - std::min(hordeRatio, allianceRatio)) /
            (largestRatio * 2);

        if (unavailableRatio != 0)
        {
            uint32 const adjusted =
                static_cast<uint32>(
                    availableChars -
                    availableChars * unavailableRatio);

            availableChars = std::max<uint32>(1, adjusted);
        }
    }

    return availableChars;
}

void RandomPlayerbotFactory::CreateRandomBots()
{
    /* multi-thread here is meaningless? since the async db operations */

    // Read the current playerbots.conf blacklist once for this creation run.
    LoadRandomBotConfBlacklist(true);

    if (sPlayerbotAIConfig.deleteRandomBotAccounts)
    {
        std::vector<uint32> botAccounts;
        std::vector<uint32> botFriends;

        // Calculates the total number of required accounts.
        uint32 totalAccountCount = CalculateTotalAccountCount();

        for (uint32 accountNumber = 0; accountNumber < totalAccountCount; ++accountNumber)
        {
            std::ostringstream out;
            out << sPlayerbotAIConfig.randomBotAccountPrefix << accountNumber;
            std::string const accountName = out.str();

            if (uint32 accountId = AccountMgr::GetId(accountName))
                botAccounts.push_back(accountId);
        }

        LOG_INFO("playerbots", "Deleting all random bot characters and accounts...");

        // First execute all the cleanup SQL commands
        // Clear playerbots_random_bots and playerbots_account_type
        PlayerbotsDatabase.Execute("DELETE FROM playerbots_random_bots");
        PlayerbotsDatabase.Execute("DELETE FROM playerbots_account_type");

        // Get the database names dynamically
        std::string loginDBName = LoginDatabase.GetConnectionInfo()->database;
        std::string characterDBName = CharacterDatabase.GetConnectionInfo()->database;

        // Delete all characters from bot accounts
        CharacterDatabase.Execute("DELETE FROM characters WHERE account IN (SELECT id FROM " + loginDBName + ".account WHERE username LIKE '{}%%')",
            sPlayerbotAIConfig.randomBotAccountPrefix.c_str());

        // Wait for the characters to be deleted before proceeding to dependent deletes
        while (CharacterDatabase.QueueSize())
        {
            std::this_thread::sleep_for(1s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));    // Extra 100ms fixed delay for safety.

        // Clean up orphaned entries in playerbots_guild_tasks
        PlayerbotsDatabase.Execute("DELETE FROM playerbots_guild_tasks WHERE owner NOT IN (SELECT guid FROM " + characterDBName + ".characters)");

        // Clean up orphaned entries in playerbots_db_store
        PlayerbotsDatabase.Execute("DELETE FROM playerbots_db_store WHERE guid NOT IN (SELECT guid FROM " + characterDBName + ".characters WHERE account IN (SELECT id FROM " + loginDBName + ".account WHERE username NOT LIKE '{}%%'))",
            sPlayerbotAIConfig.randomBotAccountPrefix.c_str());

        // Clean up orphaned records in character-related tables
        CharacterDatabase.Execute("DELETE FROM arena_team_member WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM arena_team WHERE arenaTeamId NOT IN (SELECT arenaTeamId FROM arena_team_member)");
        CharacterDatabase.Execute("DELETE FROM character_account_data WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_achievement WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_achievement_progress WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_action WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_arena_stats WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_aura WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_entry_point WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_glyphs WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_homebind WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_inventory WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM item_instance WHERE owner_guid NOT IN (SELECT guid FROM characters) AND owner_guid > 0");

        // Clean up pet data
        CharacterDatabase.Execute("DELETE FROM character_pet WHERE owner NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM pet_aura WHERE guid NOT IN (SELECT id FROM character_pet)");
        CharacterDatabase.Execute("DELETE FROM pet_spell WHERE guid NOT IN (SELECT id FROM character_pet)");
        CharacterDatabase.Execute("DELETE FROM pet_spell_cooldown WHERE guid NOT IN (SELECT id FROM character_pet)");

        // Clean up character data
        CharacterDatabase.Execute("DELETE FROM character_queststatus WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_queststatus_rewarded WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_reputation WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_skills WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_social WHERE friend NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_spell WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_spell_cooldown WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM character_talent WHERE guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM corpse WHERE guid NOT IN (SELECT guid FROM characters)");

        // Clean up group data
        CharacterDatabase.Execute("DELETE FROM `groups` WHERE leaderGuid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM group_member WHERE memberGuid NOT IN (SELECT guid FROM characters)");

        // Clean up mail
        CharacterDatabase.Execute("DELETE FROM mail WHERE receiver NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM mail_items WHERE receiver NOT IN (SELECT guid FROM characters)");

        // Clean up guild data
        CharacterDatabase.Execute("DELETE FROM guild WHERE leaderguid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM guild_bank_eventlog WHERE guildid NOT IN (SELECT guildid FROM guild)");
        CharacterDatabase.Execute("DELETE FROM guild_member WHERE guildid NOT IN (SELECT guildid FROM guild) OR guid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM guild_rank WHERE guildid NOT IN (SELECT guildid FROM guild)");

        // Clean up petition data
        CharacterDatabase.Execute("DELETE FROM petition WHERE ownerguid NOT IN (SELECT guid FROM characters)");
        CharacterDatabase.Execute("DELETE FROM petition_sign WHERE ownerguid NOT IN (SELECT guid FROM characters) OR playerguid NOT IN (SELECT guid FROM characters)");

        // Finally, delete the bot accounts themselves
        LOG_INFO("playerbots", "Deleting random bot accounts...");
        QueryResult results = LoginDatabase.Query("SELECT id FROM account WHERE username LIKE '{}%%'",
                                             sPlayerbotAIConfig.randomBotAccountPrefix.c_str());
        int32 deletion_count = 0;
        if (results)
        {
            do
            {
                Field* fields = results->Fetch();
                uint32 accId = fields[0].Get<uint32>();
                LOG_DEBUG("playerbots", "Deleting account accID: {}({})...", accId, ++deletion_count);
                AccountMgr::DeleteAccount(accId);
            } while (results->NextRow());
        }

        uint32 timer = getMSTime();

        // After ALL deletions, make sure data is commited to DB
        LoginDatabase.Execute("COMMIT");
        CharacterDatabase.Execute("COMMIT");
        PlayerbotsDatabase.Execute("COMMIT");

        // Wait for all pending database operations to complete
        while (LoginDatabase.QueueSize() || CharacterDatabase.QueueSize() || PlayerbotsDatabase.QueueSize())
        {
            std::this_thread::sleep_for(1s);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));    // Extra 100ms fixed delay for safety.

        // Flush tables to ensure all data in memory are written to disk
        LoginDatabase.Execute("FLUSH TABLES");
        CharacterDatabase.Execute("FLUSH TABLES");
        PlayerbotsDatabase.Execute("FLUSH TABLES");

        LOG_INFO("playerbots", ">> Random bot accounts and data deleted in {} ms", GetMSTimeDiffToNow(timer));
        LOG_INFO("playerbots", "Please reset the AiPlayerbot.DeleteRandomBotAccounts to 0 and restart the server...");
        World::StopNow(SHUTDOWN_EXIT_CODE);
        return;
    }

    LOG_INFO("playerbots", "Creating random bot accounts...");
    std::unordered_map<NameRaceAndGender, std::vector<std::string>> nameCache;
    std::vector<std::future<void>> account_creations;
    int account_creation = 0;

    // Calculates the total number of required accounts.
    uint32 totalAccountCount = CalculateTotalAccountCount();
    uint32 timer = getMSTime();

    for (uint32 accountNumber = 0; accountNumber < totalAccountCount; ++accountNumber)
    {
        std::ostringstream out;
        out << sPlayerbotAIConfig.randomBotAccountPrefix << accountNumber;
        std::string const accountName = out.str();

        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_ACCOUNT_ID_BY_USERNAME);
        stmt->SetData(0, accountName);
        PreparedQueryResult result = LoginDatabase.Query(stmt);
        if (result)
        {
            continue;
        }
        account_creation++;
        std::string password = "";
        if (sPlayerbotAIConfig.randomBotRandomPassword)
        {
            for (int i = 0; i < 10; i++)
            {
                password += (char)urand('!', 'z');
            }
        }
        else
            password = accountName;

        sAccountMgr->CreateAccount(accountName, password);

        LOG_DEBUG("playerbots", "Account {} created for random bots", accountName.c_str());
    }
    if (account_creation)
    {
        LOG_INFO("playerbots", "Waiting for {} accounts loading into database ({} queries)...", account_creation, LoginDatabase.QueueSize());
        /* wait for async accounts create to make character create correctly */

        while (LoginDatabase.QueueSize())
        {
            std::this_thread::sleep_for(1s);
        }
        LOG_INFO("playerbots", ">> {} Accounts loaded into database in {} ms", account_creation, GetMSTimeDiffToNow(timer));
    }

    LOG_INFO("playerbots", "Creating random bot characters...");
    uint32 totalRandomBotChars = 0;
    std::vector<std::pair<Player*, uint32>> playerBots;
    std::vector<WorldSession*> sessionBots;
    int bot_creation = 0;
    timer = getMSTime();
    bool nameCached = false;

    // Keep the generated RNDbot character pool at the configured target instead of filling every
    // automatically-created account to 10 characters. Account calculation is intentionally left
    // unchanged so there are still enough accounts to hold the requested number of usable bots.
    uint32 targetRndBotChars = sPlayerbotAIConfig.maxRandomBots;
    if (sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        targetRndBotChars *= sPlayerbotAIConfig.periodicOnlineOfflineRatio;

    uint32 availableCharsPerAccount = CalculateAvailableCharsPerAccount();
    uint32 neededRndBotAccounts = 0;
    if (targetRndBotChars && availableCharsPerAccount)
        neededRndBotAccounts = (targetRndBotChars + availableCharsPerAccount - 1) / availableCharsPerAccount;
    neededRndBotAccounts = std::min(neededRndBotAccounts, totalAccountCount);

    // Count characters already present in the accounts that AssignAccountTypes() will reserve as
    // RNDbot accounts (the lowest-numbered rndbot accounts). This makes restarts idempotent: missing
    // characters may be filled, but the factory will not keep adding duplicates on every restart.
    uint32 rndBotCharCount = 0;
    for (uint32 accountNumber = 0; accountNumber < neededRndBotAccounts; ++accountNumber)
    {
        std::ostringstream out;
        out << sPlayerbotAIConfig.randomBotAccountPrefix << accountNumber;
        uint32 accountId = AccountMgr::GetId(out.str());
        if (accountId)
            rndBotCharCount += AccountMgr::GetCharactersCount(accountId);
    }

    if (rndBotCharCount > targetRndBotChars)
    {
        LOG_WARN("playerbots",
            "Existing RNDbot character count ({}) is already above the configured target ({}). "
            "No existing characters will be deleted automatically.",
            rndBotCharCount, targetRndBotChars);
    }

    for (uint32 accountNumber = 0; accountNumber < totalAccountCount; ++accountNumber)
    {
        std::ostringstream out;
        out << sPlayerbotAIConfig.randomBotAccountPrefix << accountNumber;
        std::string const accountName = out.str();

        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_ACCOUNT_ID_BY_USERNAME);
        stmt->SetData(0, accountName);
        PreparedQueryResult result = LoginDatabase.Query(stmt);
        if (!result)
            continue;

        Field* fields = result->Fetch();
        uint32 accountId = fields[0].Get<uint32>();

        sPlayerbotAIConfig.randomBotAccounts.push_back(accountId);

        uint32 count = AccountMgr::GetCharactersCount(accountId);
        bool isRndBotAccount = accountNumber < neededRndBotAccounts;

        // Once the exact configured RNDbot pool size is reached, leave any remaining RNDbot accounts
        // partially filled. They are still valid accounts and are needed because account allocation is
        // calculated conservatively (for example, 400 non-DK bots require 45 accounts).
        if (isRndBotAccount && rndBotCharCount >= targetRndBotChars)
            continue;

        if (count >= 10)
            continue;

        // Remember which classes already exist on this account. The previous implementation restarted
        // its class loop at Warrior whenever an account had fewer than 10 characters, which created
        // duplicate Warrior/Paladin/Hunter/etc. characters on later server starts.
        std::set<uint8> existingClasses;
        CharacterDatabasePreparedStatement* charsStmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARS_BY_ACCOUNT_ID);
        charsStmt->SetData(0, accountId);
        if (PreparedQueryResult charsResult = CharacterDatabase.Query(charsStmt))
        {
            do
            {
                Field* charFields = charsResult->Fetch();
                existingClasses.insert(charFields[1].Get<uint8>());
            } while (charsResult->NextRow());
        }

        if (!nameCached)
        {
            nameCached = true;
            LOG_INFO("playerbots", "Creating cache for names per gender and race...");
            QueryResult result = CharacterDatabase.Query("SELECT name, gender FROM playerbots_names");
            if (!result)
            {
                LOG_ERROR("playerbots", "No more unused names left");
                return;
            }
            do
            {
                Field* fields = result->Fetch();
                std::string name = fields[0].Get<std::string>();
                NameRaceAndGender raceAndGender = static_cast<NameRaceAndGender>(fields[1].Get<uint8>());
                if (sObjectMgr->CheckPlayerName(name) == CHAR_NAME_SUCCESS)
                {
                    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
                    stmt->SetData(0, name);

                    if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
                        continue;

                    nameCache[raceAndGender].push_back(name);
                }

            } while (result->NextRow());
        }

        LOG_DEBUG("playerbots", "Creating random bot characters for account: [{}/{}]", accountNumber + 1, totalAccountCount);
        RandomPlayerbotFactory factory;

        WorldSession* session = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
                                                time_t(0), LOCALE_enUS, 0, false, false, 0, true);
        sessionBots.push_back(session);

        for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
        {
            if (count >= 10)
                break;

            if (isRndBotAccount && rndBotCharCount >= targetRndBotChars)
                break;

            // skip nonexistent classes
            if (!((1 << (cls - 1)) & CLASSMASK_ALL_PLAYABLE) || !sChrClassesStore.LookupEntry(cls))
                continue;

            // skip disabled with config classes
            if ((1 << (cls - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK))
                continue;

            // If DK login is disabled, do not generate DKs in the RNDbot pool at all. This keeps the
            // generated character count aligned with CalculateAvailableCharsPerAccount().
            if (isRndBotAccount && sPlayerbotAIConfig.disableDeathKnightLogin && cls == CLASS_DEATH_KNIGHT)
                continue;

            // One character per class per account. Never create duplicate classes just because an
            // account is partially filled.
            if (existingClasses.find(cls) != existingClasses.end())
                continue;

            if (!HasAnyAllowedRaceForClass(cls))
                continue;

            Player* playerBot = factory.CreateRandomBot(session, cls, nameCache);
            if (!playerBot)
            {
                LOG_ERROR("playerbots", "Fail to create character for account {}", accountId);
                continue;
            }

            playerBot->SaveToDB(true, false);
            sCharacterCache->AddCharacterCacheEntry(playerBot->GetGUID(), accountId, playerBot->GetName(),
                                                    playerBot->getGender(), playerBot->getRace(),
                                                    playerBot->getClass(), playerBot->GetLevel());
            playerBot->CleanupsBeforeDelete();
            delete playerBot;

            existingClasses.insert(cls);
            ++count;
            ++bot_creation;
            if (isRndBotAccount)
                ++rndBotCharCount;
        }
    }

    if (bot_creation)
    {
        LOG_INFO("playerbots", "Waiting for {} characters loading into database ({} queries)...", bot_creation, CharacterDatabase.QueueSize());
        /* wait for characters load into database, or characters will fail to loggin */
        while (CharacterDatabase.QueueSize())
        {
            std::this_thread::sleep_for(1s);
        }
        LOG_INFO("playerbots", ">> {} Characters loaded into database in {} ms", bot_creation, GetMSTimeDiffToNow(timer));
    }

    for (WorldSession* session : sessionBots)
        delete session;

    for (uint32 accountId : sPlayerbotAIConfig.randomBotAccounts)
    {
        totalRandomBotChars += AccountMgr::GetCharactersCount(accountId);
    }

    LOG_INFO("server.loading", ">> {} random bot accounts with {} characters available",
            sPlayerbotAIConfig.randomBotAccounts.size(), totalRandomBotChars);
}

std::string const RandomPlayerbotFactory::CreateRandomGuildName()
{
    std::string guildName = "";

    QueryResult result = CharacterDatabase.Query("SELECT MAX(name_id) FROM playerbots_guild_names");
    if (!result)
    {
        LOG_ERROR("playerbots", "No more names left for random guilds");
        return guildName;
    }

    Field* fields = result->Fetch();
    uint32 maxId = fields[0].Get<uint32>();

    uint32 id = urand(0, maxId);
    result = CharacterDatabase.Query(
        "SELECT n.name FROM playerbots_guild_names n "
        "LEFT OUTER JOIN guild e ON e.name = n.name WHERE e.guildid IS NULL AND n.name_id >= {} LIMIT 1",
        id);
    if (!result)
    {
        LOG_ERROR("playerbots", "No more names left for random guilds");
        return guildName;
    }

    fields = result->Fetch();
    guildName = fields[0].Get<std::string>();

    return guildName;
}

bool RandomPlayerbotFactory::IsBotArenaTeam(ArenaTeam const* team)
{
    if (!team)
        return false;

    ObjectGuid captainGuid = team->GetCaptain();
    if (!captainGuid || !captainGuid.IsPlayer())
        return false;

    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(captainGuid);
    return accountId && sPlayerbotAIConfig.IsInRandomAccountList(accountId);
}

void RandomPlayerbotFactory::LoadArenaTeamData()
{
    _configTargets = {
        {ARENA_TYPE_2v2, sPlayerbotAIConfig.randomBotArenaTeam2v2Count},
        {ARENA_TYPE_3v3, sPlayerbotAIConfig.randomBotArenaTeam3v3Count},
        {ARENA_TYPE_5v5, sPlayerbotAIConfig.randomBotArenaTeam5v5Count},
    };

    _botArenaTeamRegistry.clear();

    for (auto const& [id, team] : sArenaTeamMgr->GetArenaTeams())
    {
        if (!IsBotArenaTeam(team))
            continue;

        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(team->GetCaptain());
        if (!entry)
            continue;

        ArenaType type = static_cast<ArenaType>(team->GetType());
        _botArenaTeamRegistry[type].push_back(id);
    }

    _availableArenaTeamNames.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT n.name FROM playerbots_arena_team_names n "
        "LEFT OUTER JOIN arena_team e ON e.name = n.name "
        "WHERE e.arenateamid IS NULL");

    if (!result)
    {
        LOG_WARN("playerbots", "No arena team names left in playerbots_arena_team_names");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        _availableArenaTeamNames.push_back(fields[0].Get<std::string>());
    } while (result->NextRow());

    for (size_t i = _availableArenaTeamNames.size() - 1; i > 0; --i)
    {
        size_t j = urand(0, i);
        std::swap(_availableArenaTeamNames[i], _availableArenaTeamNames[j]);
    }

    LOG_INFO("playerbots", "Loaded {} available arena team names", _availableArenaTeamNames.size());
}

void RandomPlayerbotFactory::AssignBotToArenaTeam(Player* bot)
{
    if (!sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
        return;

    if (sPlayerbotAIConfig.deleteRandomBotArenaTeams)
        return;

    if (bot->GetLevel() < 70)
        return;

    for (uint32 arena_slot = 0; arena_slot < MAX_ARENA_SLOT; ++arena_slot)
    {
        if (bot->GetArenaTeamId(arena_slot))
            return;
    }

    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<ArenaTeamAssignOperation>(bot->GetGUID()));
}

void RandomPlayerbotFactory::AssignBotToArenaTeamInternal(Player* bot)
{
    // Check if bot has team, only one per bot to avoid queue conflicts
    for (uint32 arena_slot = 0; arena_slot < MAX_ARENA_SLOT; ++arena_slot)
    {
        if (bot->GetArenaTeamId(arena_slot) ||
            sCharacterCache->GetCharacterArenaTeamIdByGuid(bot->GetGUID(), arena_slot))
            return;
    }

    TeamId const botTeam = bot->GetTeamId();

    // Randomize type order so no single type starves the others
    std::array<ArenaType, 3> order{ARENA_TYPE_2v2, ARENA_TYPE_3v3, ARENA_TYPE_5v5};
    for (size_t i = order.size() - 1; i > 0; --i)
        std::swap(order[i], order[urand(0, i)]);

    std::vector<ArenaTeam*> candidates;
    for (ArenaType type : order)
        CollectJoinableBotArenaTeams(type, botTeam, candidates);

    for (size_t i = candidates.size(); i > 0; --i)
    {
        size_t const index = urand(0, i - 1);
        ArenaTeam* team = candidates[index];
        candidates[index] = candidates[i - 1];

        if (!team->AddMember(bot->GetGUID()))
        {
            LOG_DEBUG("playerbots", "Failed to add bot {} to arena team '{}', trying next candidate",
                      bot->GetName(), team->GetName());
            continue;
        }

        if (team->GetMembersSize() >= static_cast<uint32>(team->GetType()))
        {
            uint32 teamRating = team->GetRating();
            team->SetRatingForAll(teamRating);

            // Keep MMR synchronized with team rating so matchmaking reflects artificial bot strength
            // (1000-2000 range) instead of the global CONFIG_ARENA_START_MATCHMAKER_RATING default.
            for (auto& member : team->GetMembers())
            {
                member.MatchMakerRating = member.PersonalRating;
                member.MaxMMR = std::max(member.MaxMMR, member.PersonalRating);
            }
            team->SaveToDB(true);
        }
        return;
    }

    // No joinable team available, create one if under target count
    for (ArenaType type : order)
    {
        if (GetBotArenaTeamCount(type) < _configTargets[type])
        {
            CreateBotArenaTeam(bot, type);
            return;
        }
    }
}

void RandomPlayerbotFactory::CreateBotArenaTeam(Player* bot, ArenaType type)
{
    std::string teamName = CreateRandomArenaTeamName();
    if (teamName.empty())
        return;

    ArenaTeam* arenateam = new ArenaTeam();
    if (!arenateam->Create(bot->GetGUID(), type, teamName, 0, 0, 0, 0, 0))
    {
        LOG_ERROR("playerbots", "Error creating arena team {}", teamName);
        delete arenateam;
        _availableArenaTeamNames.push_back(std::move(teamName));
        return;
    }

    arenateam->SetRatingForAll(
        urand(sPlayerbotAIConfig.randomBotArenaTeamMinRating, sPlayerbotAIConfig.randomBotArenaTeamMaxRating));

    uint32 backgroundColor = urand(0xFF000000, 0xFFFFFFFF);
    uint32 emblemStyle = urand(0, 101);
    uint32 emblemColor = urand(0xFF000000, 0xFFFFFFFF);
    uint32 borderStyle = urand(0, 5);
    uint32 borderColor = urand(0xFF000000, 0xFFFFFFFF);
    arenateam->SetEmblem(backgroundColor, emblemStyle, emblemColor, borderStyle, borderColor);

    arenateam->SaveToDB();
    sArenaTeamMgr->AddArenaTeam(arenateam);
    _botArenaTeamRegistry[type].push_back(arenateam->GetId());

    LOG_DEBUG("playerbots", "Created {}v{} arena team '{}' with captain {}",
              type, type, teamName, bot->GetName());
}

uint32 RandomPlayerbotFactory::GetBotArenaTeamCount(ArenaType type)
{
    auto it = _botArenaTeamRegistry.find(type);
    return it != _botArenaTeamRegistry.end() ? static_cast<uint32>(it->second.size()) : 0;
}

void RandomPlayerbotFactory::CollectJoinableBotArenaTeams(ArenaType type, TeamId faction, std::vector<ArenaTeam*>& out)
{
    auto it = _botArenaTeamRegistry.find(type);
    if (it == _botArenaTeamRegistry.end())
        return;

    uint32 const capacity = static_cast<uint32>(type);
    for (uint32 teamId : it->second)
    {
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
        if (!team || team->GetMembersSize() >= capacity)
            continue;

        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(team->GetCaptain());
        if (entry && Player::TeamIdForRace(entry->Race) == faction)
            out.push_back(team);
    }
}

void RandomPlayerbotFactory::DeleteBotArenaTeams()
{
    LOG_INFO("playerbots", "Deleting random bot arena teams...");

    std::vector<uint32> teamsToDisband;
    for (auto const& [id, arenateam] : sArenaTeamMgr->GetArenaTeams())
    {
        if (IsBotArenaTeam(arenateam))
            teamsToDisband.push_back(id);
    }

    for (uint32 teamId : teamsToDisband)
    {
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
        if (team)
            team->Disband(nullptr);
    }

    _botArenaTeamRegistry.clear();
    LOG_INFO("playerbots", "Deleted {} random bot arena teams", teamsToDisband.size());
}

std::string RandomPlayerbotFactory::CreateRandomArenaTeamName()
{
    if (_availableArenaTeamNames.empty())
    {
        LOG_ERROR("playerbots", "No more names left for random arena teams");
        return "";
    }

    std::string name = std::move(_availableArenaTeamNames.back());
    _availableArenaTeamNames.pop_back();
    return name;
}
