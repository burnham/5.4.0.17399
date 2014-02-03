/*
 * Copyright (C) 2008-2013 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/** \file
    \ingroup u2w
*/

#include "WorldSocket.h"                                    // must be first to make ACE happy with ACE includes in it
#include <zlib.h>
#include "Common.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Player.h"
#include "Vehicle.h"
#include "ObjectMgr.h"
#include "GuildMgr.h"
#include "Group.h"
#include "Guild.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "BattlegroundMgr.h"
#include "OutdoorPvPMgr.h"
#include "MapManager.h"
#include "SocialMgr.h"
#include "zlib.h"
#include "ScriptMgr.h"
#include "Transport.h"
#include "WardenWin.h"
#include "WardenMac.h"

namespace {

std::string const DefaultPlayerName = "<none>";

} // namespace

bool MapSessionFilter::Process(WorldPacket* packet)
{
    Opcodes opcode = DropHighBytes(packet->GetOpcode());
    OpcodeHandler const* opHandle = opcodeTable[opcode];

    //let's check if our opcode can be really processed in Map::Update()
    if (opHandle->ProcessingPlace == PROCESS_INPLACE)
        return true;

    //we do not process thread-unsafe packets
    if (opHandle->ProcessingPlace == PROCESS_THREADUNSAFE)
        return false;

    Player* player = m_pSession->GetPlayer();
    if (!player)
        return false;

    //in Map::Update() we do not process packets where player is not in world!
    return player->IsInWorld();
}

//we should process ALL packets when player is not in world/logged in
//OR packet handler is not thread-safe!
bool WorldSessionFilter::Process(WorldPacket* packet)
{
    Opcodes opcode = DropHighBytes(packet->GetOpcode());
    OpcodeHandler const* opHandle = opcodeTable[opcode];
    //check if packet handler is supposed to be safe
    if (opHandle->ProcessingPlace == PROCESS_INPLACE)
        return true;

    //thread-unsafe packets should be processed in World::UpdateSessions()
    if (opHandle->ProcessingPlace == PROCESS_THREADUNSAFE)
        return true;

    //no player attached? -> our client! ^^
    Player* player = m_pSession->GetPlayer();
    if (!player)
        return true;

    //lets process all packets for non-in-the-world player
    return (player->IsInWorld() == false);
}

/// WorldSession constructor
WorldSession::WorldSession(uint32 id, WorldSocket* sock, AccountTypes sec, uint8 expansion, time_t mute_time, LocaleConstant locale, uint32 recruiter, bool isARecruiter):
    m_muteTime(mute_time),
    m_timeOutTime(0),
    _player(NULL),
    m_Socket(sock),
    _security(sec),
    _accountId(id),
    m_expansion(expansion),
    _warden(NULL),
    _logoutTime(0),
    m_inQueue(false),
    m_playerLoading(false),
    m_playerLogout(false),
    m_playerRecentlyLogout(false),
    m_playerSave(false),
    m_sessionDbcLocale(sWorld->GetAvailableDbcLocale(locale)),
    m_sessionDbLocaleIndex(locale),
    m_latency(0),
    m_TutorialsChanged(false),
    _filterAddonMessages(false),
    recruiterId(recruiter),
    isRecruiter(isARecruiter),
    timeLastWhoCommand(0)
{
    if (sock)
    {
        m_Address = sock->GetRemoteAddress();
        sock->AddReference();
        ResetTimeOutTime();
        LoginDatabase.PExecute("UPDATE account SET online = 1 WHERE id = %u;", GetAccountId());     // One-time query
    }

    InitializeQueryCallbackParameters();

    _compressionStream = new z_stream();
    _compressionStream->zalloc = (alloc_func)NULL;
    _compressionStream->zfree = (free_func)NULL;
    _compressionStream->opaque = (voidpf)NULL;
    _compressionStream->avail_in = 0;
    _compressionStream->next_in = NULL;
    int32 z_res = deflateInit(_compressionStream, sWorld->getIntConfig(CONFIG_COMPRESSION));
    if (z_res != Z_OK)
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "Can't initialize packet compression (zlib: deflateInit) Error code: %i (%s)", z_res, zError(z_res));
        return;
    }
}

/// WorldSession destructor
WorldSession::~WorldSession()
{
    ///- unload player if not unloaded
    if (_player)
        LogoutPlayer (true);

    /// - If have unclosed socket, close it
    if (m_Socket)
    {
        m_Socket->CloseSocket();
        m_Socket->RemoveReference();
        m_Socket = NULL;
    }

    if (_warden)
        delete _warden;

    ///- empty incoming packet queue
    WorldPacket* packet = NULL;
    while (_recvQueue.next(packet))
        delete packet;

    LoginDatabase.PExecute("UPDATE account SET online = 0 WHERE id = %u;", GetAccountId());     // One-time query

    int32 z_res = deflateEnd(_compressionStream);
    if (z_res != Z_OK && z_res != Z_DATA_ERROR) // Z_DATA_ERROR signals that internal state was BUSY
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "Can't close packet compression stream (zlib: deflateEnd) Error code: %i (%s)", z_res, zError(z_res));
        return;
    }

    delete _compressionStream;
}

std::string const & WorldSession::GetPlayerName() const
{
    return _player != NULL ? _player->GetName() : DefaultPlayerName;
}

std::string WorldSession::GetPlayerInfo() const
{
    std::ostringstream ss;

    ss << "[Player: " << GetPlayerName()
       << " (Guid: " << (_player != NULL ? _player->GetGUID() : 0)
       << ", Account: " << GetAccountId() << ")]";

    return ss.str();
}

/// Get player guid if available. Use for logging purposes only
uint32 WorldSession::GetGuidLow() const
{
    return GetPlayer() ? GetPlayer()->GetGUIDLow() : 0;
}

/// Send a packet to the client
void WorldSession::SendPacket(WorldPacket const* packet, bool forced /*= false*/)
{
    if (!m_Socket)
        return;

    if (packet->GetOpcode() == NULL_OPCODE)
    {
        sLog->outError(LOG_FILTER_OPCODES, "Prevented sending of NULL_OPCODE to %s", GetPlayerInfo().c_str());
        return;
    }
    else if (packet->GetOpcode() == UNKNOWN_OPCODE)
    {
        sLog->outError(LOG_FILTER_OPCODES, "Prevented sending of UNKNOWN_OPCODE to %s", GetPlayerInfo().c_str());
        return;
    }

    if (!forced)
    {
        OpcodeHandler const* handler = opcodeTable[packet->GetOpcode()];
        if (!handler || handler->Status == STATUS_UNHANDLED)
        {
            sLog->outError(LOG_FILTER_OPCODES, "Prevented sending disabled opcode %s to %s", GetOpcodeNameForLogging(packet->GetOpcode()).c_str(), GetPlayerInfo().c_str());
            return;
        }
    }

#ifdef TRINITY_DEBUG
    // Code for network use statistic
    static uint64 sendPacketCount = 0;
    static uint64 sendPacketBytes = 0;

    static time_t firstTime = time(NULL);
    static time_t lastTime = firstTime;                     // next 60 secs start time

    static uint64 sendLastPacketCount = 0;
    static uint64 sendLastPacketBytes = 0;

    time_t cur_time = time(NULL);

    if ((cur_time - lastTime) < 60)
    {
        sendPacketCount+=1;
        sendPacketBytes+=packet->size();

        sendLastPacketCount+=1;
        sendLastPacketBytes+=packet->size();
    }
    else
    {
        uint64 minTime = uint64(cur_time - lastTime);
        uint64 fullTime = uint64(lastTime - firstTime);
        sLog->outInfo(LOG_FILTER_GENERAL, "Send all time packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f time: %u", sendPacketCount, sendPacketBytes, float(sendPacketCount)/fullTime, float(sendPacketBytes)/fullTime, uint32(fullTime));
        sLog->outInfo(LOG_FILTER_GENERAL, "Send last min packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f", sendLastPacketCount, sendLastPacketBytes, float(sendLastPacketCount)/minTime, float(sendLastPacketBytes)/minTime);

        lastTime = cur_time;
        sendLastPacketCount = 1;
        sendLastPacketBytes = packet->wpos();               // wpos is real written size
    }
#endif                                                      // !TRINITY_DEBUG

    if (m_Socket->SendPacket(*packet) == -1)
        m_Socket->CloseSocket();
}

/// Add an incoming packet to the queue
void WorldSession::QueuePacket(WorldPacket* new_packet)
{
    _recvQueue.add(new_packet);
}

/// Logging helper for unexpected opcodes
void WorldSession::LogUnexpectedOpcode(WorldPacket* packet, const char* status, const char *reason)
{
    sLog->outError(LOG_FILTER_OPCODES, "Received unexpected opcode %s Status: %s Reason: %s from %s",
        GetOpcodeNameForLogging(packet->GetOpcode()).c_str(), status, reason, GetPlayerInfo().c_str());
}

/// Logging helper for unexpected opcodes
void WorldSession::LogUnprocessedTail(WorldPacket* packet)
{
    if (!sLog->ShouldLog(LOG_FILTER_OPCODES, LOG_LEVEL_TRACE) || packet->rpos() >= packet->wpos())
        return;

    sLog->outTrace(LOG_FILTER_OPCODES, "Unprocessed tail data (read stop at %u from %u) Opcode %s from %s",
        uint32(packet->rpos()), uint32(packet->wpos()), GetOpcodeNameForLogging(packet->GetOpcode()).c_str(), GetPlayerInfo().c_str());
    packet->print_storage();
}

/// Update the WorldSession (triggered by World update)
bool WorldSession::Update(uint32 diff, PacketFilter& updater)
{
    /// Update Timeout timer.
    UpdateTimeOutTime(diff);

    ///- Before we process anything:
    /// If necessary, kick the player from the character select screen
    if (IsConnectionIdle())
        m_Socket->CloseSocket();

    ///- Retrieve packets from the receive queue and call the appropriate handlers
    /// not process packets if socket already closed
    WorldPacket* packet = NULL;
    //! Delete packet after processing by default
    bool deletePacket = true;
    //! To prevent infinite loop
    WorldPacket* firstDelayedPacket = NULL;
    //! If _recvQueue.peek() == firstDelayedPacket it means that in this Update call, we've processed all
    //! *properly timed* packets, and we're now at the part of the queue where we find
    //! delayed packets that were re-enqueued due to improper timing. To prevent an infinite
    //! loop caused by re-enqueueing the same packets over and over again, we stop updating this session
    //! and continue updating others. The re-enqueued packets will be handled in the next Update call for this session.
    while (m_Socket && !m_Socket->IsClosed() &&
            !_recvQueue.empty() && _recvQueue.peek(true) != firstDelayedPacket &&
            _recvQueue.next(packet, updater))
    {
        OpcodeHandler const* opHandle = opcodeTable[packet->GetOpcode()];

        try
        {
            switch (opHandle->Status)
            {
                case STATUS_LOGGEDIN:
                    if (!_player)
                    {
                        // skip STATUS_LOGGEDIN opcode unexpected errors if player logout sometime ago - this can be network lag delayed packets
                        //! If player didn't log out a while ago, it means packets are being sent while the server does not recognize
                        //! the client to be in world yet. We will re-add the packets to the bottom of the queue and process them later.
                        if (!m_playerRecentlyLogout)
                        {
                            //! Prevent infinite loop
                            if (!firstDelayedPacket)
                                firstDelayedPacket = packet;
                            //! Because checking a bool is faster than reallocating memory
                            deletePacket = false;
                            QueuePacket(packet);
                            //! Log
                                sLog->outDebug(LOG_FILTER_NETWORKIO, "Re-enqueueing packet with opcode %s with with status STATUS_LOGGEDIN. "
                                    "Player is currently not in world yet.", GetOpcodeNameForLogging(packet->GetOpcode()).c_str());
                        }
                    }
                    else if (_player->IsInWorld())
                    {
                        sScriptMgr->OnPacketReceive(m_Socket, WorldPacket(*packet));
                        (this->*opHandle->Handler)(*packet);
                        LogUnprocessedTail(packet);
                    }
                    // lag can cause STATUS_LOGGEDIN opcodes to arrive after the player started a transfer
                    break;
                case STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT:
                    if (!_player && !m_playerRecentlyLogout && !m_playerLogout) // There's a short delay between _player = null and m_playerRecentlyLogout = true during logout
                        LogUnexpectedOpcode(packet, "STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT",
                            "the player has not logged in yet and not recently logout");
                    else
                    {
                        // not expected _player or must checked in packet hanlder
                        sScriptMgr->OnPacketReceive(m_Socket, WorldPacket(*packet));
                        (this->*opHandle->Handler)(*packet);
                        LogUnprocessedTail(packet);
                    }
                    break;
                case STATUS_TRANSFER:
                    if (!_player)
                        LogUnexpectedOpcode(packet, "STATUS_TRANSFER", "the player has not logged in yet");
                    else if (_player->IsInWorld())
                        LogUnexpectedOpcode(packet, "STATUS_TRANSFER", "the player is still in world");
                    else
                    {
                        sScriptMgr->OnPacketReceive(m_Socket, WorldPacket(*packet));
                        (this->*opHandle->Handler)(*packet);
                        LogUnprocessedTail(packet);
                    }
                    break;
                case STATUS_AUTHED:
                    // prevent cheating with skip queue wait
                    if (m_inQueue)
                    {
                        LogUnexpectedOpcode(packet, "STATUS_AUTHED", "the player not pass queue yet");
                        break;
                    }

                    // some auth opcodes can be recieved before STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT opcodes
                    // however when we recieve CMSG_CHAR_ENUM we are surely no longer during the logout process.
                    if (packet->GetOpcode() == CMSG_CHAR_ENUM)
                        m_playerRecentlyLogout = false;

                    sScriptMgr->OnPacketReceive(m_Socket, WorldPacket(*packet));
                    (this->*opHandle->Handler)(*packet);
                    LogUnprocessedTail(packet);
                    break;
                case STATUS_NEVER:
                        sLog->outError(LOG_FILTER_OPCODES, "Received not allowed opcode %s from %s", GetOpcodeNameForLogging(packet->GetOpcode()).c_str()
                            , GetPlayerInfo().c_str());
                    break;
                case STATUS_UNHANDLED:
                        sLog->outError(LOG_FILTER_OPCODES, "Received not handled opcode %s from %s", GetOpcodeNameForLogging(packet->GetOpcode()).c_str()
                            , GetPlayerInfo().c_str());
                    break;
            }
        }
        catch(ByteBufferException &)
        {
            sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::Update ByteBufferException occured while parsing a packet (opcode: %u) from client %s, accountid=%i. Skipped packet.",
                    packet->GetOpcode(), GetRemoteAddress().c_str(), GetAccountId());
            packet->hexlike();
        }

        if (deletePacket)
            delete packet;
    }

    if (m_Socket && !m_Socket->IsClosed() && _warden)
        _warden->Update();

    ProcessQueryCallbacks();

    //check if we are safe to proceed with logout
    //logout procedure should happen only in World::UpdateSessions() method!!!
    if (updater.ProcessLogout())
    {
        time_t currTime = time(NULL);
        ///- If necessary, log the player out
        if (ShouldLogOut(currTime) && !m_playerLoading)
            LogoutPlayer(true);

        if (m_Socket && GetPlayer() && _warden)
            _warden->Update();

        ///- Cleanup socket pointer if need
        if (m_Socket && m_Socket->IsClosed())
        {
            m_Socket->RemoveReference();
            m_Socket = NULL;
        }

        if (!m_Socket)
            return false;                                       //Will remove this session from the world session map
    }

    return true;
}

/// %Log the player out
void WorldSession::LogoutPlayer(bool Save)
{
    // finish pending transfers before starting the logout
    while (_player && _player->IsBeingTeleportedFar())
        HandleMoveWorldportAckOpcode();

    m_playerLogout = true;
    m_playerSave = Save;

    if (_player)
    {
        if (uint64 lguid = _player->GetLootGUID())
            DoLootRelease(lguid);

        ///- If the player just died before logging out, make him appear as a ghost
        //FIXME: logout must be delayed in case lost connection with client in time of combat
        if (_player->GetDeathTimer())
        {
            _player->getHostileRefManager().deleteReferences();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }
        else if (!_player->getAttackers().empty())
        {
            // build set of player who attack _player or who have pet attacking of _player
            std::set<Player*> aset;
            for (Unit::AttackerSet::const_iterator itr = _player->getAttackers().begin(); itr != _player->getAttackers().end(); ++itr)
            {
                Unit* owner = (*itr)->GetOwner();           // including player controlled case
                if (owner && owner->GetTypeId() == TYPEID_PLAYER)
                    aset.insert(owner->ToPlayer());
                else if ((*itr)->GetTypeId() == TYPEID_PLAYER)
                    aset.insert((Player*)(*itr));
            }

            // CombatStop() method is removing all attackers from the AttackerSet
            // That is why it must be AFTER building current set of attackers
            _player->CombatStop();
            _player->getHostileRefManager().setOnlineOfflineState(false);
            _player->RemoveAllAurasOnDeath();
            _player->SetPvPDeath(!aset.empty());
            _player->KillPlayer();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();

            // give honor to all attackers from set like group case
            for (std::set<Player*>::const_iterator itr = aset.begin(); itr != aset.end(); ++itr)
                (*itr)->RewardHonor(_player, aset.size());

            // give bg rewards and update counters like kill by first from attackers
            // this can't be called for all attackers.
            if (!aset.empty())
                if (Battleground* bg = _player->GetBattleground())
                    bg->HandleKillPlayer(_player, *aset.begin());
        }
        else if (_player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
        {
            // this will kill character by SPELL_AURA_SPIRIT_OF_REDEMPTION
            _player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            _player->KillPlayer();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }
        else if (_player->HasPendingBind())
        {
            _player->RepopAtGraveyard();
            _player->SetPendingBind(0, 0);
        }

        //drop a flag if player is carrying it
        if (Battleground* bg = _player->GetBattleground())
            bg->EventPlayerLoggedOut(_player);

        ///- Teleport to home if the player is in an invalid instance
        if (!_player->m_InstanceValid && !_player->isGameMaster())
            _player->TeleportTo(_player->m_homebindMapId, _player->m_homebindX, _player->m_homebindY, _player->m_homebindZ, _player->GetOrientation());

        sOutdoorPvPMgr->HandlePlayerLeaveZone(_player, _player->GetZoneId());

        for (int i=0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            if (BattlegroundQueueTypeId bgQueueTypeId = _player->GetBattlegroundQueueTypeId(i))
            {
                _player->RemoveBattlegroundQueueId(bgQueueTypeId);
                BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);
                queue.RemovePlayer(_player->GetGUID(), true);
            }
        }

        // Repop at GraveYard or other player far teleport will prevent saving player because of not present map
        // Teleport player immediately for correct player save
        while (_player->IsBeingTeleportedFar())
            HandleMoveWorldportAckOpcode();

        ///- If the player is in a guild, update the guild roster and broadcast a logout message to other guild members
        if (Guild* guild = sGuildMgr->GetGuildById(_player->GetGuildId()))
            guild->HandleMemberLogout(this);

        ///- Remove pet
        _player->RemovePet(NULL, PET_SAVE_AS_CURRENT, true);

        ///- empty buyback items and save the player in the database
        // some save parts only correctly work in case player present in map/player_lists (pets, etc)
        if (Save)
        {
            uint32 eslot;
            for (int j = BUYBACK_SLOT_START; j < BUYBACK_SLOT_END; ++j)
            {
                eslot = j - BUYBACK_SLOT_START;
                _player->SetUInt64Value(PLAYER_FIELD_VENDORBUYBACK_SLOT_1 + (eslot * 2), 0);
                _player->SetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1 + eslot, 0);
                _player->SetUInt32Value(PLAYER_FIELD_BUYBACK_TIMESTAMP_1 + eslot, 0);
            }
            _player->SaveToDB();
        }

        ///- Leave all channels before player delete...
        _player->CleanupChannels();

        ///- If the player is in a group (or invited), remove him. If the group if then only 1 person, disband the group.
        _player->UninviteFromGroup();

        // remove player from the group if he is:
        // a) in group; b) not in raid group; c) logging out normally (not being kicked or disconnected)
        if (_player->GetGroup() && !_player->GetGroup()->isRaidGroup() && m_Socket)
            _player->RemoveFromGroup();

        //! Send update to group and reset stored max enchanting level
        if (_player->GetGroup())
        {
            _player->GetGroup()->SendUpdate();
            _player->GetGroup()->ResetMaxEnchantingLevel();
        }

        //! Broadcast a logout message to the player's friends
        sSocialMgr->SendFriendStatus(_player, FRIEND_OFFLINE, _player->GetGUIDLow(), true);
        sSocialMgr->RemovePlayerSocial(_player->GetGUIDLow());

        //! Call script hook before deletion
        sScriptMgr->OnPlayerLogout(_player);

        //! Remove the player from the world
        // the player may not be in the world when logging out
        // e.g if he got disconnected during a transfer to another map
        // calls to GetMap in this case may cause crashes
        _player->CleanupsBeforeDelete();
        sLog->outInfo(LOG_FILTER_CHARACTER, "Account: %d (IP: %s) Logout Character:[%s] (GUID: %u) Level: %d",
            GetAccountId(), GetRemoteAddress().c_str(), _player->GetName().c_str(), _player->GetGUIDLow(), _player->getLevel());
        if (Map* _map = _player->FindMap())
            _map->RemovePlayerFromMap(_player, true);

        SetPlayer(NULL); //! Pointer already deleted during RemovePlayerFromMap

        //! Send the 'logout complete' packet to the client
        //! Client will respond by sending 3x CMSG_CANCEL_TRADE, which we currently dont handle
        WorldPacket data(SMSG_LOGOUT_COMPLETE, 0);
        SendPacket(&data);
        sLog->outDebug(LOG_FILTER_NETWORKIO, "SESSION: Sent SMSG_LOGOUT_COMPLETE Message");

        //! Since each account can only have one online character at any given time, ensure all characters for active account are marked as offline
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_ACCOUNT_ONLINE);
        stmt->setUInt32(0, GetAccountId());
        CharacterDatabase.Execute(stmt);
    }

    m_playerLogout = false;
    m_playerSave = false;
    m_playerRecentlyLogout = true;
    LogoutRequest(0);
}

/// Kick a player out of the World
void WorldSession::KickPlayer()
{
    if (m_Socket)
        m_Socket->CloseSocket();
}

void WorldSession::SendNotification(const char *format, ...)
{
    if (format)
    {
        va_list ap;
        char szStr[1024];
        szStr[0] = '\0';
        va_start(ap, format);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        size_t len = strlen(szStr);
        WorldPacket data(SMSG_NOTIFICATION, 2 + len);
        data.WriteBits(len, 13);
        data.FlushBits();
        data.append(szStr, len);
        SendPacket(&data);
    }
}

void WorldSession::SendNotification(uint32 string_id, ...)
{
    char const* format = GetTrinityString(string_id);
    if (format)
    {
        va_list ap;
        char szStr[1024];
        szStr[0] = '\0';
        va_start(ap, string_id);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        size_t len = strlen(szStr);
        WorldPacket data(SMSG_NOTIFICATION, 2 + len);
        data.WriteBits(len, 13);
        data.FlushBits();
        data.append(szStr, len);
        SendPacket(&data);
    }
}

const char *WorldSession::GetTrinityString(int32 entry) const
{
    return sObjectMgr->GetTrinityString(entry, GetSessionDbLocaleIndex());
}

void WorldSession::Handle_NULL(WorldPacket& recvPacket)
{
    sLog->outError(LOG_FILTER_OPCODES, "Received unhandled opcode %s from %s"
        , GetOpcodeNameForLogging(recvPacket.GetOpcode()).c_str(), GetPlayerInfo().c_str());
}

void WorldSession::Handle_EarlyProccess(WorldPacket& recvPacket)
{
    sLog->outError(LOG_FILTER_OPCODES, "Received opcode %s that must be processed in WorldSocket::OnRead from %s"
        , GetOpcodeNameForLogging(recvPacket.GetOpcode()).c_str(), GetPlayerInfo().c_str());
}

void WorldSession::Handle_ServerSide(WorldPacket& recvPacket)
{
    sLog->outError(LOG_FILTER_OPCODES, "Received server-side opcode %s from %s"
        , GetOpcodeNameForLogging(recvPacket.GetOpcode()).c_str(), GetPlayerInfo().c_str());
}

void WorldSession::Handle_Deprecated(WorldPacket& recvPacket)
{
    sLog->outError(LOG_FILTER_OPCODES, "Received deprecated opcode %s from %s"
        , GetOpcodeNameForLogging(recvPacket.GetOpcode()).c_str(), GetPlayerInfo().c_str());
}

void WorldSession::SendAuthWaitQue(uint32 position)
{
    if (position == 0)
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
        packet.WriteBit(0); // has queue info
        packet.WriteBit(0); // has account info
        packet.FlushBits();
        packet << uint8(AUTH_OK);
        SendPacket(&packet);
    }
    else
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 6);
        packet.WriteBit(1); // has queue info
        packet.WriteBit(0); // unk queue bool
        packet.WriteBit(0); // has account info
        packet.FlushBits();
        packet << uint32(position);
        packet << uint8(AUTH_WAIT_QUEUE);
		SendPacket(&packet);
    }
}

void WorldSession::LoadGlobalAccountData()
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_DATA);
    stmt->setUInt32(0, GetAccountId());
    LoadAccountData(CharacterDatabase.Query(stmt), GLOBAL_CACHE_MASK);
}

void WorldSession::LoadAccountData(PreparedQueryResult result, uint32 mask)
{
    for (uint32 i = 0; i < NUM_ACCOUNT_DATA_TYPES; ++i)
        if (mask & (1 << i))
            m_accountData[i] = AccountData();

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 type = fields[0].GetUInt8();
        if (type >= NUM_ACCOUNT_DATA_TYPES)
        {
            sLog->outError(LOG_FILTER_GENERAL, "Table `%s` have invalid account data type (%u), ignore.",
                mask == GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        if ((mask & (1 << type)) == 0)
        {
            sLog->outError(LOG_FILTER_GENERAL, "Table `%s` have non appropriate for table  account data type (%u), ignore.",
                mask == GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        m_accountData[type].Time = time_t(fields[1].GetUInt32());
        m_accountData[type].Data = fields[2].GetString();
    }
    while (result->NextRow());
}

void WorldSession::SetAccountData(AccountDataType type, time_t tm, std::string const& data)
{
    uint32 id = 0;
    uint32 index = 0;
    if ((1 << type) & GLOBAL_CACHE_MASK)
    {
        id = GetAccountId();
        index = CHAR_REP_ACCOUNT_DATA;
    }
    else
    {
        // _player can be NULL and packet received after logout but m_GUID still store correct guid
        if (!m_GUIDLow)
            return;

        id = m_GUIDLow;
        index = CHAR_REP_PLAYER_ACCOUNT_DATA;
    }

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(index);
    stmt->setUInt32(0, id);
    stmt->setUInt8 (1, type);
    stmt->setUInt32(2, uint32(tm));
    stmt->setString(3, data);
    CharacterDatabase.Execute(stmt);

    m_accountData[type].Time = tm;
    m_accountData[type].Data = data;
}

void WorldSession::SendAccountDataTimes(uint32 mask)
{
    WorldPacket data(SMSG_ACCOUNT_DATA_TIMES, 4 + 1 + 4 + NUM_ACCOUNT_DATA_TYPES * 4);
    data << uint32(time(NULL));                             // Server time
    data << uint8(1);
    data << uint32(mask);                                   // type mask
    for (uint32 i = 0; i < NUM_ACCOUNT_DATA_TYPES; ++i)
        if (mask & (1 << i))
            data << uint32(GetAccountData(AccountDataType(i))->Time);// also unix time
    SendPacket(&data);

    std::string l_TimeZone1 = "Europe/Paris";
    std::string l_TimeZone2 = "Europe/Paris";

    WorldPacket data2(SMSG_SET_TIME_ZONE);
    data2.WriteBits(l_TimeZone1.length(), 9);
    data2.WriteBits(l_TimeZone2.length(), 9);
    data2.FlushBits();

    data2.WriteString(l_TimeZone1);
    data2.WriteString(l_TimeZone2);

    SendPacket(&data2);
}

void WorldSession::LoadTutorialsData()
{
    memset(m_Tutorials, 0, sizeof(uint32) * MAX_ACCOUNT_TUTORIAL_VALUES);

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_TUTORIALS);
    stmt->setUInt32(0, GetAccountId());
    if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
            m_Tutorials[i] = (*result)[i].GetUInt32();

    m_TutorialsChanged = false;
}

void WorldSession::SendTutorialsData()
{
    WorldPacket data(SMSG_TUTORIAL_FLAGS, 4 * MAX_ACCOUNT_TUTORIAL_VALUES);
    for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
        data << m_Tutorials[i];
    SendPacket(&data);
}

void WorldSession::SaveTutorialsData(SQLTransaction &trans)
{
    if (!m_TutorialsChanged)
        return;

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_HAS_TUTORIALS);
    stmt->setUInt32(0, GetAccountId());
    bool hasTutorials = !CharacterDatabase.Query(stmt).null();
    // Modify data in DB
    stmt = CharacterDatabase.GetPreparedStatement(hasTutorials ? CHAR_UPD_TUTORIALS : CHAR_INS_TUTORIALS);
    for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
        stmt->setUInt32(i, m_Tutorials[i]);
    stmt->setUInt32(MAX_ACCOUNT_TUTORIAL_VALUES, GetAccountId());
    trans->Append(stmt);

    m_TutorialsChanged = false;
}

void WorldSession::ReadAddonsInfo(WorldPacket &data)
{
    if (data.rpos() + 4 > data.size())
        return;

    uint32 size;
    data >> size;

    if (!size)
        return;

    if (size > 0xFFFFF)
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::ReadAddonsInfo addon info too big, size %u", size);
        return;
    }

    uLongf uSize = size;

    uint32 pos = data.rpos();

    ByteBuffer addonInfo;
    addonInfo.resize(size);

    if (uncompress(addonInfo.contents(), &uSize, data.contents() + pos, data.size() - pos) == Z_OK)
    {
        uint32 addonsCount;
        addonInfo >> addonsCount;                         // addons count

        for (uint32 i = 0; i < addonsCount; ++i)
        {
            std::string addonName;
            uint8 enabled;
            uint32 crc, version;

            // check next addon data format correctness
            if (addonInfo.rpos() + 1 > addonInfo.size())
                return;

            addonInfo >> addonName;

            addonInfo >> enabled >> crc >> version;

            AddonInfo addon(addonName, enabled, crc, 2, true, version);

            SavedAddon const* savedAddon = AddonMgr::GetAddonInfo(addonName);
            if (savedAddon)
            {
                bool match = true;

                if (addon.CRC != savedAddon->CRC)
                    match = false;
            }
            else
            {
                AddonMgr::SaveAddon(addon);

                sLog->outInfo(LOG_FILTER_NETWORKIO, "ADDON: %s (0x%x) was not known, saving...", addon.Name.c_str(), addon.CRC);
            }

            // TODO: Find out when to not use CRC/pubkey, and other possible states.
            m_addonsList.push_back(addon);
        }

        uint32 currentTime;
        addonInfo >> currentTime;

        if (addonInfo.rpos() != addonInfo.size())
            sLog->outDebug(LOG_FILTER_NETWORKIO, "packet under-read!");
    }
    else
        sLog->outError(LOG_FILTER_NETWORKIO, "Addon packet uncompress error!");
}

void WorldSession::SendAddonsInfo()
{
    uint8 addonPublicKey[256] =
    {
        0x1E, 0x64, 0x6E, 0x0F, 0x7C, 0x45, 0xF9, 0x8F, 0xB9, 0x0E, 0x9A, 0x8F, 0x44, 0xC2, 0xCB, 0x10, 
        0xDF, 0x0D, 0x50, 0x2A, 0xE0, 0x17, 0xC3, 0x7A, 0xCE, 0xC6, 0x8F, 0x03, 0x43, 0xB6, 0x44, 0x56, 
        0x2C, 0x97, 0x92, 0x75, 0xE1, 0x2D, 0x1E, 0xBE, 0xD5, 0xCF, 0x22, 0x45, 0x87, 0xB5, 0x4A, 0x88, 
        0xF2, 0x0E, 0x56, 0xFA, 0xFE, 0x6D, 0xE1, 0xF5, 0x8F, 0x40, 0x2F, 0xF0, 0x16, 0xBB, 0xAD, 0xD6, 
        0x8E, 0xFE, 0x5A, 0x6E, 0x13, 0x5B, 0x93, 0xDA, 0x0D, 0x94, 0xA6, 0x6F, 0x15, 0xFB, 0xCC, 0x0D, 
        0xE7, 0x85, 0xA2, 0x8B, 0x72, 0xE0, 0xC3, 0xEA, 0x8C, 0x0A, 0x0E, 0xCE, 0x77, 0x91, 0xFB, 0x4B, 
        0x72, 0x11, 0xED, 0x4B, 0x1A, 0x63, 0xBE, 0x01, 0x8C, 0x22, 0x79, 0x14, 0x22, 0x37, 0x36, 0x34, 
        0xEE, 0x8C, 0x8A, 0x5D, 0xDB, 0x5D, 0x4E, 0x85, 0xED, 0xD0, 0xD8, 0x2B, 0x01, 0x9B, 0x3C, 0xAA, 
        0x9E, 0xC7, 0xA8, 0xFB, 0x49, 0x69, 0xE5, 0xBA, 0x54, 0xBA, 0xFC, 0x30, 0x3F, 0x6B, 0xD1, 0x0F, 
        0xC6, 0xEF, 0x3E, 0x50, 0xFF, 0x04, 0x68, 0x89, 0x3D, 0xAD, 0x16, 0x64, 0x1B, 0xB9, 0x80, 0xA8, 
        0xC7, 0x05, 0x79, 0xB9, 0x57, 0xB3, 0x5A, 0xCA, 0x23, 0x34, 0xEF, 0x95, 0x64, 0xF3, 0x9E, 0x51,
        0x86, 0xFC, 0xA3, 0x1C, 0xBB, 0x8B, 0x98, 0x0E, 0x07, 0x20, 0xF0, 0x29, 0x84, 0x8B, 0xC3, 0x50,
        0x31, 0x50, 0x8B, 0x3C, 0x32, 0xFB, 0x10, 0x85, 0xF7, 0x46, 0x44, 0x0F, 0x02, 0x2C, 0x5D, 0x08,
        0xD2, 0x9C, 0x6F, 0x28, 0xCF, 0x69, 0xB4, 0xA3, 0x40, 0x7A, 0x76, 0x5E, 0x54, 0x7E, 0x45, 0xCB, 
        0xC3, 0x18, 0xD4, 0x31, 0x68, 0xD7, 0x34, 0x10, 0x84, 0x42, 0x3D, 0x36, 0x0A, 0x59, 0xBC, 0xD7, 
        0x2D, 0x24, 0xCF, 0x81, 0xF0, 0x79, 0xDD, 0x57, 0xD9, 0xB7, 0x5A, 0x77, 0xC8, 0x85, 0x48, 0x1B
    };

   WorldPacket data(SMSG_ADDON_INFO, 40);

    data.WriteBits(m_addonsList.size(), 23);

    for (AddonsList::iterator itr = m_addonsList.begin(); itr != m_addonsList.end(); ++itr)
    {
        data.WriteBit(0); //usesURL
        data.WriteBit(1);//itr->UsePublicKeyOrCRC);
        //if(usesURL)
        //    data.WriteBits(URL.length(), 8);
        data.WriteBit(1);
    }


    //AddonMgr::BannedAddonList const* bannedAddons = AddonMgr::GetBannedAddons();
    data.WriteBits(0, 18); //bannedAddons->size()

    data.FlushBits();

    for (AddonsList::iterator itr = m_addonsList.begin(); itr != m_addonsList.end(); ++itr)
    {
        //if (usesURL)
        //    data << uint8(0); // URL

       // if (itr->UsePublicKeyOrCRC)
            data.append(addonPublicKey, sizeof(addonPublicKey));

        data << uint8(1);
        data << uint32(itr->Version);
        
        
        data << uint8(itr->State);
    }

    m_addonsList.clear();

    //for (AddonMgr::BannedAddonList::const_iterator itr = bannedAddons->begin(); itr != bannedAddons->end(); ++itr)
    //{
    //    data << uint32(itr->Id);
    //    data.append(itr->NameMD5, sizeof(itr->NameMD5));
    //    data.append(itr->VersionMD5, sizeof(itr->VersionMD5));
    //    data << uint32(itr->Timestamp);
    //    data << uint32(1);  // IsBanned
    //}

    SendPacket(&data);
}

bool WorldSession::IsAddonRegistered(const std::string& prefix) const
{
    if (!_filterAddonMessages) // if we have hit the softcap (64) nothing should be filtered
        return true;

    if (_registeredAddonPrefixes.empty())
        return false;

    std::vector<std::string>::const_iterator itr = std::find(_registeredAddonPrefixes.begin(), _registeredAddonPrefixes.end(), prefix);
    return itr != _registeredAddonPrefixes.end();
}

void WorldSession::HandleUnregisterAddonPrefixesOpcode(WorldPacket& /*recvPacket*/) // empty packet
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_UNREGISTER_ALL_ADDON_PREFIXES");

    _registeredAddonPrefixes.clear();
}

void WorldSession::HandleAddonRegisteredPrefixesOpcode(WorldPacket& recvPacket)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_ADDON_REGISTERED_PREFIXES");

    // This is always sent after CMSG_UNREGISTER_ALL_ADDON_PREFIXES

    uint32 count = recvPacket.ReadBits(25);

    if (count > REGISTERED_ADDON_PREFIX_SOFTCAP)
    {
        // if we have hit the softcap (64) nothing should be filtered
        _filterAddonMessages = false;
        recvPacket.rfinish();
        return;
    }

    std::vector<uint8> lengths(count);
    for (uint32 i = 0; i < count; ++i)
        lengths[i] = recvPacket.ReadBits(5);

    for (uint32 i = 0; i < count; ++i)
        _registeredAddonPrefixes.push_back(recvPacket.ReadString(lengths[i]));

    if (_registeredAddonPrefixes.size() > REGISTERED_ADDON_PREFIX_SOFTCAP) // shouldn't happen
    {
        _filterAddonMessages = false;
        return;
    }

    _filterAddonMessages = true;
}

void WorldSession::SetPlayer(Player* player)
{
    _player = player;

    // set m_GUID that can be used while player loggined and later until m_playerRecentlyLogout not reset
    if (_player)
        m_GUIDLow = _player->GetGUIDLow();
}

void WorldSession::InitializeQueryCallbackParameters()
{
    // Callback parameters that have pointers in them should be properly
    // initialized to NULL here.
    _charCreateCallback.SetParam(NULL);
}

void WorldSession::ProcessQueryCallbacks()
{
    PreparedQueryResult result;

    //! HandleCharEnumOpcode
    if (_charEnumCallback.ready())
    {
        _charEnumCallback.get(result);
        HandleCharEnum(result);
        _charEnumCallback.cancel();
    }

    if (_charCreateCallback.IsReady())
    {
        _charCreateCallback.GetResult(result);
        HandleCharCreateCallback(result, _charCreateCallback.GetParam());
        // Don't call FreeResult() here, the callback handler will do that depending on the events in the callback chain
    }

    //! HandlePlayerLoginOpcode
    if (_charLoginCallback.ready())
    {
        SQLQueryHolder* param;
        _charLoginCallback.get(param);
        HandlePlayerLogin((LoginQueryHolder*)param);
        _charLoginCallback.cancel();
    }

    //! HandleAddFriendOpcode
    if (_addFriendCallback.IsReady())
    {
        std::string param = _addFriendCallback.GetParam();
        _addFriendCallback.GetResult(result);
        HandleAddFriendOpcodeCallBack(result, param);
        _addFriendCallback.FreeResult();
    }

    //- HandleCharRenameOpcode
    if (_charRenameCallback.IsReady())
    {
        std::string param = _charRenameCallback.GetParam();
        _charRenameCallback.GetResult(result);
        HandleChangePlayerNameOpcodeCallBack(result, param);
        _charRenameCallback.FreeResult();
    }

    //- HandleCharAddIgnoreOpcode
    if (_addIgnoreCallback.ready())
    {
        _addIgnoreCallback.get(result);
        HandleAddIgnoreOpcodeCallBack(result);
        _addIgnoreCallback.cancel();
    }

    //- SendStabledPet
    if (_sendStabledPetCallback.IsReady())
    {
        uint64 param = _sendStabledPetCallback.GetParam();
        _sendStabledPetCallback.GetResult(result);
        SendStablePetCallback(result, param);
        _sendStabledPetCallback.FreeResult();
    }

    //- HandleStablePet
    if (_stablePetCallback.ready())
    {
        _stablePetCallback.get(result);
        HandleStablePetCallback(result);
        _stablePetCallback.cancel();
    }

    //- HandleUnstablePet
    if (_unstablePetCallback.IsReady())
    {
        uint32 param = _unstablePetCallback.GetParam();
        _unstablePetCallback.GetResult(result);
        HandleUnstablePetCallback(result, param);
        _unstablePetCallback.FreeResult();
    }

    //- HandleStableSwapPet
    if (_stableSwapCallback.IsReady())
    {
        uint32 param = _stableSwapCallback.GetParam();
        _stableSwapCallback.GetResult(result);
        HandleStableSwapPetCallback(result, param);
        _stableSwapCallback.FreeResult();
    }
}

void WorldSession::InitWarden(BigNumber* k, std::string const& os)
{
    if (os == "Win")
    {
        _warden = new WardenWin();
        _warden->Init(this, k);
    }
    else if (os == "OSX")
    {
        // Disabled as it is causing the client to crash
        // _warden = new WardenMac();
        // _warden->Init(this, k);
    }
}