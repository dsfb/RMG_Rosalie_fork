/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "CachedRomHeaderAndSettings.hpp"
#include "Directories.hpp"
#include "RomSettings.hpp"
#include "RomHeader.hpp"
#include "Library.hpp"
#include "File.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

//
// Local Defines
//

#define ROMHEADER_NAME_LEN 256
#define GOODNAME_LEN 256
#define MD5_LEN 33
#define GAMEID_LEN 5
#define REGION_LEN 18

#ifdef _WIN32
#define CACHE_FILE_MAGIC "RMGCoreHeaderAndSettingsCacheWindows_09"
#else // Linux
#define CACHE_FILE_MAGIC "RMGCoreHeaderAndSettingsCacheLinux_09"
#endif // _WIN32
#define CACHE_FILE_ITEMS_MAX 250000

//
// Local Structures
//

struct l_CacheEntry
{
    std::filesystem::path fileName;
    CoreFileTime fileTime;

    bool valid;

    CoreRomType     type;
    CoreRomHeader   header;
    CoreRomSettings settings;
    CoreRomSettings defaultSettings;
};

//
// Local Variables
//

static bool                      l_CacheEntriesChanged = false;
static std::vector<l_CacheEntry> l_CacheEntries;

//
// Internal Functions
//

static std::filesystem::path get_cache_file_name(void)
{
    std::filesystem::path file;

    file = CoreGetUserCacheDirectory();
    file += CORE_DIR_SEPERATOR_STR;
    file += "RomHeaderAndSettingsCache.cache";

    return file;
}

static std::vector<l_CacheEntry>::iterator get_cache_entry_iter(const std::filesystem::path& file, bool checkFileTime = true)
{
    CoreFileTime fileTime = (checkFileTime ? CoreGetFileTime(file) : 0);

    auto predicate = [file, fileTime, checkFileTime](const auto& entry)
    {
        return entry.fileName == file &&
                (!checkFileTime || entry.fileTime == fileTime);
    };

    return std::find_if(l_CacheEntries.begin(), l_CacheEntries.end(), predicate);
}

static void add_cache_entry(const std::filesystem::path& file, CoreRomType type, 
                            const CoreRomHeader& header, const CoreRomSettings& defaultSettings,
                            const CoreRomSettings& settings)
{
    l_CacheEntry cacheEntry;

    // try to find existing entry with same filename,
    // when found, remove it from the cache
    auto iter = get_cache_entry_iter(file, false);
    if (iter != l_CacheEntries.end())
    {
        l_CacheEntries.erase(iter);
    }
    else if (l_CacheEntries.size() >= CACHE_FILE_ITEMS_MAX)
    { // delete first item when we're over the item limit
        l_CacheEntries.erase(l_CacheEntries.begin());
    }

    cacheEntry.fileName = file;
    cacheEntry.fileTime = CoreGetFileTime(file);
    cacheEntry.type     = type;
    cacheEntry.header   = header;
    cacheEntry.settings = settings;
    cacheEntry.defaultSettings = defaultSettings;
    cacheEntry.valid    = true;

    l_CacheEntries.push_back(cacheEntry);
    l_CacheEntriesChanged = true;
}

static void add_invalid_cache_entry(const std::filesystem::path& file)
{
    l_CacheEntry cacheEntry;

    // try to find existing entry with same filename,
    // when found, remove it from the cache
    auto iter = get_cache_entry_iter(file, false);
    if (iter != l_CacheEntries.end())
    {
        l_CacheEntries.erase(iter);
    }
    else if (l_CacheEntries.size() >= CACHE_FILE_ITEMS_MAX)
    { // delete first item when we're over the item limit
        l_CacheEntries.erase(l_CacheEntries.begin());
    }

    cacheEntry.fileName = file;
    cacheEntry.fileTime = CoreGetFileTime(file);
    cacheEntry.valid    = false;

    l_CacheEntries.push_back(cacheEntry);
    l_CacheEntriesChanged = true;
}

//
// Exported Functions
//

CORE_EXPORT void CoreReadRomHeaderAndSettingsCache(void)
{
    std::ifstream inputStream;
    char magicBuf[sizeof(CACHE_FILE_MAGIC)];
    wchar_t fileNameBuf[CORE_DIR_MAX_LEN];
    char headerNameBuf[ROMHEADER_NAME_LEN];
    char gameIDBuf[GAMEID_LEN];
    char regionBuf[REGION_LEN];
    char goodNameBuf[GOODNAME_LEN];
    char md5Buf[MD5_LEN];
    uint32_t size;
    l_CacheEntry cacheEntry;

    inputStream.open(get_cache_file_name(), std::ios::binary);
    if (!inputStream.good())
    {
        return;
    }

    // when magic doesn't match, don't read cache file
    inputStream.read((char*)magicBuf, sizeof(CACHE_FILE_MAGIC));
    if (std::string(magicBuf) != std::string(CACHE_FILE_MAGIC))
    {
        inputStream.close();
        return;
    }

    // read entry count
    size = l_CacheEntries.size();
    inputStream.read((char*)&size, sizeof(size));

    // reserve items
    l_CacheEntries.reserve(size);

    // read all file entries
#define FREAD(x) inputStream.read((char*)&x, sizeof(x))
#define FREAD_STR(x, y) inputStream.read((char*)x, y)
    while (!inputStream.eof())
    {
        // reset state
        size = 0;
        cacheEntry = {};
        memset(fileNameBuf, 0, sizeof(fileNameBuf));
        memset(headerNameBuf, 0, sizeof(headerNameBuf));
        memset(gameIDBuf, 0, sizeof(gameIDBuf));
        memset(regionBuf, 0, sizeof(regionBuf));
        memset(goodNameBuf, 0, sizeof(goodNameBuf));
        memset(md5Buf, 0, sizeof(md5Buf));

        // file info
        FREAD(size);
        FREAD_STR(fileNameBuf, size);
        cacheEntry.fileName = std::filesystem::path(fileNameBuf);
        FREAD(cacheEntry.fileTime);
        // validity
        FREAD(cacheEntry.valid);
        // invalid entries have less data
        // so we don't need to read further
        if (!cacheEntry.valid)
        {
            l_CacheEntries.push_back(cacheEntry);
            continue;
        }
        // type
        FREAD(cacheEntry.type);
        // header
        FREAD(size);
        FREAD_STR(headerNameBuf, size);
        FREAD(size);
        FREAD_STR(gameIDBuf, size);
        FREAD(size);
        FREAD_STR(regionBuf, size);
        cacheEntry.header.Name = std::string(headerNameBuf);
        cacheEntry.header.GameID = std::string(gameIDBuf);
        cacheEntry.header.Region = std::string(regionBuf);
        FREAD(cacheEntry.header.CRC1);
        FREAD(cacheEntry.header.CRC2);
        FREAD(cacheEntry.header.CountryCode);
        FREAD(cacheEntry.header.SystemType);
        // shared settings
        FREAD(size);
        FREAD_STR(goodNameBuf, size);
        FREAD(size);
        FREAD_STR(md5Buf, size);
        cacheEntry.defaultSettings.GoodName = std::string(goodNameBuf);
        cacheEntry.defaultSettings.MD5 = std::string(md5Buf);
        cacheEntry.settings.GoodName = std::string(goodNameBuf);
        cacheEntry.settings.MD5 = std::string(md5Buf);
        // default settings
        FREAD(cacheEntry.defaultSettings.SaveType);
        FREAD(cacheEntry.defaultSettings.DisableExtraMem);
        FREAD(cacheEntry.defaultSettings.TransferPak);
        FREAD(cacheEntry.defaultSettings.CountPerOp);
        FREAD(cacheEntry.defaultSettings.SiDMADuration);
        // current settings
        FREAD(cacheEntry.settings.SaveType);
        FREAD(cacheEntry.settings.DisableExtraMem);
        FREAD(cacheEntry.settings.TransferPak);
        FREAD(cacheEntry.settings.CountPerOp);
        FREAD(cacheEntry.settings.SiDMADuration);

        // add to cached entries
        l_CacheEntries.push_back(cacheEntry);
    }
#undef FREAD
#undef FREAD_STR

    inputStream.close();
}

CORE_EXPORT bool CoreSaveRomHeaderAndSettingsCache(void)
{
    std::ofstream outputStream;
    wchar_t fileNameBuf[CORE_DIR_MAX_LEN];
    char headerNameBuf[ROMHEADER_NAME_LEN];
    char gameIDBuf[GAMEID_LEN];
    char regionBuf[REGION_LEN];
    char goodNameBuf[GOODNAME_LEN];
    char md5Buf[MD5_LEN];
    uint32_t size;
    l_CacheEntry cacheEntry;

    // only save cache when the entries have changed
    if (!l_CacheEntriesChanged)
    {
        return true;
    }

    outputStream.open(get_cache_file_name(), std::ios::binary);
    if (!outputStream.good())
    {
        return false;
    }

    // write magic header
    outputStream.write((char*)CACHE_FILE_MAGIC, sizeof(CACHE_FILE_MAGIC));

    // write entry count
    size = l_CacheEntries.size();
    outputStream.write((char*)&size, sizeof(size));

    // write each entry in the file
#define FWRITE(x) outputStream.write((char*)&x, sizeof(x))
#define FWRITE_STR(x, y) outputStream.write((char*)x, y)
    for (auto iter = l_CacheEntries.cbegin(); iter != l_CacheEntries.end(); iter++)
    {
        cacheEntry = (*iter);

        // reset buffers
        size = 0;
        memset(fileNameBuf, 0, sizeof(fileNameBuf));
        memset(headerNameBuf, 0, sizeof(headerNameBuf));
        memset(gameIDBuf, 0, sizeof(gameIDBuf));
        memset(regionBuf, 0, sizeof(regionBuf));
        memset(goodNameBuf, 0, sizeof(goodNameBuf));
        memset(md5Buf, 0, sizeof(md5Buf));

        // copy strings into buffers
        wcsncpy(fileNameBuf, cacheEntry.fileName.wstring().c_str(), CORE_DIR_MAX_LEN);
        strncpy(headerNameBuf, cacheEntry.header.Name.c_str(), sizeof(headerNameBuf));
        strncpy(gameIDBuf, cacheEntry.header.GameID.c_str(), sizeof(gameIDBuf));
        strncpy(regionBuf, cacheEntry.header.Region.c_str(), sizeof(regionBuf));
        strncpy(goodNameBuf, cacheEntry.settings.GoodName.c_str(), sizeof(goodNameBuf));
        strncpy(md5Buf, cacheEntry.settings.MD5.c_str(), sizeof(md5Buf));

        // file info
        size = cacheEntry.fileName.wstring().size() * sizeof(wchar_t);
        FWRITE(size);
        FWRITE_STR(fileNameBuf, size);
        FWRITE(cacheEntry.fileTime);
        // validity
        FWRITE(cacheEntry.valid);
        // skip writing more data
        // when the entry is invalid
        if (!cacheEntry.valid)
        {
            continue;
        }
        // type
        FWRITE(cacheEntry.type);
        // header
        size = cacheEntry.header.Name.size();
        FWRITE(size);
        FWRITE_STR(headerNameBuf, size);
        size = cacheEntry.header.GameID.size();
        FWRITE(size);
        FWRITE_STR(gameIDBuf, size);
        size = cacheEntry.header.Region.size();
        FWRITE(size);
        FWRITE_STR(regionBuf, size);
        FWRITE(cacheEntry.header.CRC1);
        FWRITE(cacheEntry.header.CRC2);
        FWRITE(cacheEntry.header.CountryCode);
        FWRITE(cacheEntry.header.SystemType);
        // shared settings
        size = cacheEntry.settings.GoodName.size();
        FWRITE(size);
        FWRITE_STR(goodNameBuf, size);
        size = cacheEntry.settings.MD5.size();
        FWRITE(size);
        FWRITE_STR(md5Buf, size);
        // default settings
        FWRITE(cacheEntry.defaultSettings.SaveType);
        FWRITE(cacheEntry.defaultSettings.DisableExtraMem);
        FWRITE(cacheEntry.defaultSettings.TransferPak);
        FWRITE(cacheEntry.defaultSettings.CountPerOp);
        FWRITE(cacheEntry.defaultSettings.SiDMADuration);
        // current settings
        FWRITE(cacheEntry.settings.SaveType);
        FWRITE(cacheEntry.settings.DisableExtraMem);
        FWRITE(cacheEntry.settings.TransferPak);
        FWRITE(cacheEntry.settings.CountPerOp);
        FWRITE(cacheEntry.settings.SiDMADuration);
    }
#undef FWRITE
#undef FWRITE_STR

    outputStream.close();
    return true;
}

CORE_EXPORT bool CoreGetCachedRomHeaderAndSettings(std::filesystem::path file, CoreRomType* type, CoreRomHeader* header, CoreRomSettings* defaultSettings, CoreRomSettings* settings)
{
    bool ret = false;
    auto iter = get_cache_entry_iter(file);
    if (iter == l_CacheEntries.end())
    {
        CoreRomType romType;
        CoreRomHeader romHeader;
        CoreRomSettings romSettings;
        CoreRomSettings romDefaultSettings;

        // when we haven't found a cached entry,
        // we're gonna attempt to retrieve the
        // rom header and settings and add it
        // to the cache
        ret = CoreOpenRom(file) &&
                CoreGetRomType(romType) &&
                CoreGetCurrentRomHeader(romHeader) &&
                CoreGetCurrentRomSettings(romSettings) &&
                CoreGetCurrentDefaultRomSettings(romDefaultSettings);
        // always close ROM
        if (CoreHasRomOpen() && !CoreCloseRom())
        {
            ret = false;
        }
        // add file to cache
        if (ret)
        {
            if (type != nullptr)
            {
                *type = romType;
            }
            if (header != nullptr)
            {
                *header = romHeader;
            }
            if (settings != nullptr)
            {
                *settings = romSettings;
            }
            if (defaultSettings != nullptr)
            {
                *defaultSettings = romDefaultSettings;
            }

            add_cache_entry(file, romType, romHeader, romSettings, romDefaultSettings);
            return true;
        }
        else
        {
            add_invalid_cache_entry(file);
            return false;
        }
    }

    if (!(*iter).valid)
    {
        return false;
    }

    if (type != nullptr)
    {
        *type = (*iter).type;
    }
    if (header != nullptr)
    {
        *header = (*iter).header;
    }
    if (settings != nullptr)
    {
        *settings = (*iter).settings;
    }
    if (defaultSettings != nullptr)
    {
        *defaultSettings = (*iter).defaultSettings;
    }
    return true;
}

CORE_EXPORT bool CoreUpdateCachedRomHeaderAndSettings(std::filesystem::path file, CoreRomType type, CoreRomHeader header, CoreRomSettings defaultSettings, CoreRomSettings settings)
{
    l_CacheEntry cachedEntry;

    // try to find existing entry with same filename,
    // when not found, do nothing
    auto iter = get_cache_entry_iter(file, false);
    if (iter == l_CacheEntries.end())
    {
        return true;
    }

    cachedEntry = (*iter);

    // check if the cached entry needs to be updated,
    // if it does, then update the entry
    if (cachedEntry.type != type ||
        cachedEntry.header != header ||
        cachedEntry.defaultSettings != defaultSettings ||
        cachedEntry.settings != settings)
    {
        (*iter).type            = type;
        (*iter).header          = header;
        (*iter).defaultSettings = defaultSettings;
        (*iter).settings        = settings;
        (*iter).valid           = true;
        l_CacheEntriesChanged   = true;
    }

    return true;
}

CORE_EXPORT bool CoreUpdateCachedRomHeaderAndSettings(std::filesystem::path file)
{
    CoreRomType type;
    CoreRomHeader header;
    CoreRomSettings defaultSettings;
    CoreRomSettings settings;

    // try to find existing entry with same filename,
    // when not found, do nothing
    auto iter = get_cache_entry_iter(file, false);
    if (iter == l_CacheEntries.end())
    {
        return true;
    }

    // attempt to retrieve required information
    if (!CoreGetRomType(type) ||
        !CoreGetCurrentRomHeader(header) ||
        !CoreGetCurrentDefaultRomSettings(defaultSettings) ||
        !CoreGetCurrentRomSettings(settings))
    {
        return false;
    }

    return CoreUpdateCachedRomHeaderAndSettings(file, type, header, defaultSettings, settings);
}

CORE_EXPORT bool CoreClearRomHeaderAndSettingsCache(void)
{
    l_CacheEntries.clear();
    l_CacheEntriesChanged = true;
    return true;
}
