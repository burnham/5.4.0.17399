/*
 * Copyright (C) 2008-2013 TrinityCore <http://www.trinitycore.org/>
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

#include "LFGMgr.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "Player.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"

void BuildPlayerLockDungeonBlock(WorldPacket& data, LfgLockMap const& lock)
{
    data << uint32(lock.size());                           // Size of lock dungeons
    for (LfgLockMap::const_iterator it = lock.begin(); it != lock.end(); ++it)
    {
        data << uint32(it->first);                         // Dungeon entry (id + type)
        data << uint32(it->second);                        // Lock status
        data << uint32(0);                                 // Unknown 4.2.2
        data << uint32(0);                                 // Unknown 4.2.2
    }
}

void BuildPartyLockDungeonBlock(WorldPacket& data, const LfgLockPartyMap& lockMap)
{
    data << uint8(lockMap.size());
    for (LfgLockPartyMap::const_iterator it = lockMap.begin(); it != lockMap.end(); ++it)
    {
        data << uint64(it->first);                         // Player guid
        BuildPlayerLockDungeonBlock(data, it->second);
    }
}

void WorldSession::HandleLfgJoinOpcode(WorldPacket& recvData)
{
    if (!sLFGMgr->isOptionEnabled(LFG_OPTION_ENABLE_DUNGEON_FINDER | LFG_OPTION_ENABLE_RAID_BROWSER) ||
        (GetPlayer()->GetGroup() && GetPlayer()->GetGroup()->GetLeaderGUID() != GetPlayer()->GetGUID() &&
        (GetPlayer()->GetGroup()->GetMembersCount() == MAXGROUPSIZE || !GetPlayer()->GetGroup()->isLFGGroup())))
    {
        recvData.rfinish();
        return;
    }

    uint32 roles;

    recvData.read_skip<uint8>();

    for (int32 i = 0; i < 3; ++i)
        recvData.read_skip<uint32>();

    recvData >> roles;

    uint32 numDungeons = recvData.ReadBits(22);
    recvData.ReadBit();
    uint32 commentLen = recvData.ReadBits(8);

    if (!numDungeons)
    {
        sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_JOIN %s no dungeons selected", GetPlayerInfo().c_str());
        recvData.rfinish();
        return;
    }

    LfgDungeonSet newDungeons;
    for (uint32 i = 0; i < numDungeons; ++i)
    {
        uint32 dungeon;
        recvData >> dungeon;
        newDungeons.insert(dungeon);        // remove the type from the dungeon entry
    }

    std::string comment = recvData.ReadString(commentLen);

    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_JOIN %s roles: %u, Dungeons: %u, Comment: %s",
        GetPlayerInfo().c_str(), roles, uint8(newDungeons.size()), comment.c_str());

    sLFGMgr->JoinLfg(GetPlayer(), uint8(roles), newDungeons, comment);
}

void WorldSession::HandleLfgLeaveOpcode(WorldPacket&  recvData)
{
    ObjectGuid leaveGuid;
    Group* group = GetPlayer()->GetGroup();
    uint64 guid = GetPlayer()->GetGUID();
    uint64 gguid = group ? group->GetGUID() : guid;

    recvData.read_skip<uint32>();                          // Always 8
    recvData.read_skip<uint32>();                          // Join date
    recvData.read_skip<uint32>();                          // Always 3
    recvData.read_skip<uint32>();                          // Queue Id

    leaveGuid[1] = recvData.ReadBit();
    leaveGuid[6] = recvData.ReadBit();
    leaveGuid[0] = recvData.ReadBit();
    leaveGuid[7] = recvData.ReadBit();
    leaveGuid[2] = recvData.ReadBit();
    leaveGuid[4] = recvData.ReadBit();
    leaveGuid[3] = recvData.ReadBit();
    leaveGuid[5] = recvData.ReadBit();

    recvData.ReadByteSeq(leaveGuid[4]);
    recvData.ReadByteSeq(leaveGuid[5]);
    recvData.ReadByteSeq(leaveGuid[2]);
    recvData.ReadByteSeq(leaveGuid[6]);
    recvData.ReadByteSeq(leaveGuid[1]);
    recvData.ReadByteSeq(leaveGuid[3]);
    recvData.ReadByteSeq(leaveGuid[7]);
    recvData.ReadByteSeq(leaveGuid[0]);

    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_LEAVE %s in group: %u",
        GetPlayerInfo().c_str(), group ? 1 : 0);

    // Check cheating - only leader can leave the queue
    if (!group || group->GetLeaderGUID() == GetPlayer()->GetGUID())
        sLFGMgr->LeaveLfg(gguid);
}

void WorldSession::HandleLfgProposalResultOpcode(WorldPacket& recvData)
{
    uint32 proposalID;  // Proposal ID
    bool accept;

    ObjectGuid guid1;
    ObjectGuid guid2222;

    recvData.read_skip<uint32>(); // QueueID
    recvData.read_skip<uint32>(); // Time
    recvData >> proposalID;
    recvData.read_skip<uint32>();

    guid1[3] = recvData.ReadBit();
    guid1[5] = recvData.ReadBit();
    guid2222[3] = recvData.ReadBit();
    guid1[1] = recvData.ReadBit();
    guid1[0] = recvData.ReadBit();
    guid1[2] = recvData.ReadBit();
    guid2222[1] = recvData.ReadBit();
    accept =  recvData.ReadBit();
    guid2222[4] = recvData.ReadBit();
    guid1[4] = recvData.ReadBit();
    guid2222[0] = recvData.ReadBit();
    guid1[7] = recvData.ReadBit();
    guid2222[2] = recvData.ReadBit();
    guid2222[7] = recvData.ReadBit();
    guid1[6] = recvData.ReadBit();
    guid2222[6] = recvData.ReadBit();
    guid2222[5] = recvData.ReadBit();

    recvData.ReadByteSeq(guid1[2]);
    recvData.ReadByteSeq(guid1[3]);
    recvData.ReadByteSeq(guid1[4]);
    recvData.ReadByteSeq(guid2222[2]);
    recvData.ReadByteSeq(guid2222[0]);
    recvData.ReadByteSeq(guid1[6]);
    recvData.ReadByteSeq(guid2222[7]);
    recvData.ReadByteSeq(guid1[5]);
    recvData.ReadByteSeq(guid2222[1]);
    recvData.ReadByteSeq(guid1[0]);
    recvData.ReadByteSeq(guid1[7]);
    recvData.ReadByteSeq(guid2222[3]);
    recvData.ReadByteSeq(guid2222[4]);
    recvData.ReadByteSeq(guid1[1]);
    recvData.ReadByteSeq(guid2222[5]);
    recvData.ReadByteSeq(guid2222[6]);

    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_PROPOSAL_RESULT %s proposal: %u accept: %u",
        GetPlayerInfo().c_str(), proposalID, accept ? 1 : 0);
    sLFGMgr->UpdateProposal((uint32)proposalID, GetPlayer()->GetGUID(), accept);
}

void WorldSession::HandleLfgSetRolesOpcode(WorldPacket& recvData)
{
    uint32 roles;
    recvData >> roles;                                     // Player Group Roles
    uint64 guid = GetPlayer()->GetGUID();
    Group* group = GetPlayer()->GetGroup();
    if (!group)
    {
        sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_SET_ROLES %s Not in group",
            GetPlayerInfo().c_str());
        return;
    }
    uint64 gguid = group->GetGUID();
    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_SET_ROLES: Group %u, Player %s, Roles: %u",
        GUID_LOPART(gguid), GetPlayerInfo().c_str(), roles);
    sLFGMgr->UpdateRoleCheck(gguid, guid, roles);
}

void WorldSession::HandleLfgSetCommentOpcode(WorldPacket&  recvData)
{
    std::string comment;
    recvData >> comment;

    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_SET_COMMENT %s comment: %s",
        GetPlayerInfo().c_str(), comment.c_str());

    sLFGMgr->SetComment(GetPlayer()->GetGUID(), comment);
}

void WorldSession::HandleLfgSetBootVoteOpcode(WorldPacket& recvData)
{
    bool agree;                                            // Agree to kick player
    recvData >> agree;

    uint64 guid = GetPlayer()->GetGUID();
    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_SET_BOOT_VOTE %s agree: %u",
        GetPlayerInfo().c_str(), agree ? 1 : 0);
    sLFGMgr->UpdateBoot(guid, agree);
}

void WorldSession::HandleLfgTeleportOpcode(WorldPacket& recvData)
{
    bool out;
    recvData >> out;

    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_TELEPORT %s out: %u",
        GetPlayerInfo().c_str(), out ? 1 : 0);
    sLFGMgr->TeleportPlayer(GetPlayer(), out, true);
}

void WorldSession::HandleLfgPlayerLockInfoRequestOpcode(WorldPacket& /*recvData*/)
{
    uint64 guid = GetPlayer()->GetGUID();
    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_PLAYER_LOCK_INFO_REQUEST %s",
        GetPlayerInfo().c_str());

    // Get Random dungeons that can be done at a certain level and expansion
    LfgDungeonSet randomDungeons;
    uint8 level = GetPlayer()->getLevel();
    uint8 expansion = GetPlayer()->GetSession()->Expansion();

    LFGDungeonContainer& LfgDungeons = sLFGMgr->GetLFGDungeonMap();
    for (LFGDungeonContainer::const_iterator itr = LfgDungeons.begin(); itr != LfgDungeons.end(); ++itr)
    {
        LFGDungeonData const& dungeon = itr->second;
        if ((dungeon.type == LFG_TYPE_RANDOM || (dungeon.seasonal && sLFGMgr->IsSeasonActive(dungeon.id)))
            && dungeon.expansion <= expansion && dungeon.minlevel <= level && level <= dungeon.maxlevel)
            randomDungeons.insert(dungeon.Entry());
    }

    // Get player locked Dungeons
    LfgLockMap const& lock = sLFGMgr->GetLockedDungeons(guid);
    uint32 rsize = uint32(randomDungeons.size());
    uint32 lsize = uint32(lock.size());

    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_PLAYER_INFO %s", GetPlayerInfo().c_str());
    WorldPacket data(SMSG_LFG_PLAYER_INFO, 1 + rsize * (4 + 1 + 4 + 4 + 4 + 4 + 1 + 4 + 4 + 4) + 4 + lsize * (1 + 4 + 4 + 4 + 4 + 1 + 4 + 4 + 4));

    data << uint8(randomDungeons.size());                  // Random Dungeon count
    for (LfgDungeonSet::const_iterator it = randomDungeons.begin(); it != randomDungeons.end(); ++it)
    {
        data << uint32(*it);                               // Dungeon Entry (id + type)
        LfgReward const* reward = sLFGMgr->GetRandomDungeonReward(*it, level);
        Quest const* quest = NULL;
        bool done = false;
        if (reward)
        {
            quest = sObjectMgr->GetQuestTemplate(reward->firstQuest);
            if (quest)
            {
                done = !GetPlayer()->CanRewardQuest(quest, false);
                if (done)
                    quest = sObjectMgr->GetQuestTemplate(reward->otherQuest);
            }
        }

        if (quest)
        {
            data << uint8(done);
            data << uint32(quest->GetRewOrReqMoney());
            data << uint32(quest->XPValue(GetPlayer()));
            data << uint32(0);
            data << uint32(0);
            data << uint8(quest->GetRewItemsCount());
            if (quest->GetRewItemsCount())
            {
                for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
                    if (uint32 itemId = quest->RewardItemId[i])
                    {
                        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
                        data << uint32(itemId);
                        data << uint32(item ? item->DisplayInfoID : 0);
                        data << uint32(quest->RewardItemIdCount[i]);
                    }
            }
        }
        else
        {
            data << uint8(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint32(0);
            data << uint8(0);
        }
    }
    BuildPlayerLockDungeonBlock(data, lock);
    SendPacket(&data);
}

void WorldSession::HandleLfgPartyLockInfoRequestOpcode(WorldPacket&  /*recvData*/)
{
    uint64 guid = GetPlayer()->GetGUID();
    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_PARTY_LOCK_INFO_REQUEST %s", GetPlayerInfo().c_str());

    Group* group = GetPlayer()->GetGroup();
    if (!group)
        return;

    // Get the locked dungeons of the other party members
    LfgLockPartyMap lockMap;
    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* plrg = itr->getSource();
        if (!plrg)
            continue;

        uint64 pguid = plrg->GetGUID();
        if (pguid == guid)
            continue;

        lockMap[pguid] = sLFGMgr->GetLockedDungeons(pguid);
    }

    uint32 size = 0;
    for (LfgLockPartyMap::const_iterator it = lockMap.begin(); it != lockMap.end(); ++it)
        size += 8 + 4 + uint32(it->second.size()) * (4 + 4 + 4 + 4);

    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_PARTY_INFO %s", GetPlayerInfo().c_str());
    WorldPacket data(SMSG_LFG_PARTY_INFO, 1 + size);
    BuildPartyLockDungeonBlock(data, lockMap);
    SendPacket(&data);
}

void WorldSession::HandleLfrJoinOpcode(WorldPacket& recvData)
{
    uint32 entry;                                          // Raid id to search
    recvData >> entry;
    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_LFR_JOIN %s dungeon entry: %u",
        GetPlayerInfo().c_str(), entry);
    //SendLfrUpdateListOpcode(entry);
}

void WorldSession::HandleLfrLeaveOpcode(WorldPacket& recvData)
{
    uint32 dungeonId;                                      // Raid id queue to leave
    recvData >> dungeonId;
    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_LFR_LEAVE %s dungeonId: %u",
        GetPlayerInfo().c_str(), dungeonId);
    //sLFGMgr->LeaveLfr(GetPlayer(), dungeonId);
}

void WorldSession::HandleLfgGetStatus(WorldPacket& /*recvData*/)
{
    sLog->outDebug(LOG_FILTER_LFG, "CMSG_LFG_GET_STATUS %s", GetPlayerInfo().c_str());

    uint64 guid = GetPlayer()->GetGUID();
    LfgUpdateData updateData = sLFGMgr->GetLfgStatus(guid);

    if (GetPlayer()->GetGroup())
    {
        SendLfgUpdatePlayer(updateData, true);
        updateData.dungeons.clear();
        SendLfgUpdatePlayer(updateData, false);
    }
    else
    {
        SendLfgUpdatePlayer(updateData, false);
        updateData.dungeons.clear();
        SendLfgUpdatePlayer(updateData, true);
    }
}

void WorldSession::SendLfgUpdatePlayer(LfgUpdateData const& updateData, bool party)
{
    bool join = false;
    bool queued = false;
	
    uint8 size = uint8(updateData.dungeons.size());
    ObjectGuid guid = _player->GetGUID();
    time_t joinTime = sLFGMgr->GetQueueJoinTime(_player->GetGUID());
    uint32 queueId = sLFGMgr->GetQueueId(_player->GetGUID());

    switch (updateData.updateType)
    {
        case LFG_UPDATETYPE_JOIN_QUEUE_INITIAL:            // Joined queue outside the dungeon
            join = true;
            break;
        case LFG_UPDATETYPE_JOIN_QUEUE:
        case LFG_UPDATETYPE_ADDED_TO_QUEUE:                // Rolecheck Success
            join = true;
            queued = true;
            break;
        case LFG_UPDATETYPE_PROPOSAL_BEGIN:
        case LFG_UPDATETYPE_REMOVED_FROM_QUEUE_UNK_52:
            join = true;
            break;
        case LFG_UPDATETYPE_UPDATE_STATUS:
            join = updateData.state != LFG_STATE_ROLECHECK && updateData.state != LFG_STATE_NONE;
            queued = updateData.state == LFG_STATE_QUEUED;
            break;
        default:
            break;
    }

    WorldPacket data(SMSG_LFG_UPDATE_STATUS, 1 + 8 + 3 + 2 + 1 + updateData.comment.length() + 4 + 4 + 1 + 1 + 1 + 4 + size);

    for (uint8 i = 0; i < 3; ++i)
        data << uint8(0);                                  // unk - Always 0

    data << uint32(3);
    data << uint32(8);
    data << uint8(1);
    data << uint8(updateData.updateType);                  // Lfg Update type
    data << uint32(queueId);                               // Queue Id
    data << uint32(joinTime);                              // Join date

    data.WriteBit(guid[5]);
    data.WriteBits(updateData.comment.length(), 8);
    data.WriteBit(join);
    data.WriteBit(party);
    data.WriteBit(queued);                                 // Join the queue
    data.WriteBit(guid[3]);                          
    data.WriteBits(0, 24);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[6]);
    data.WriteBit(join);
    data.WriteBits(size, 22);
    data.WriteBit(size > 0);                               // Extra info

    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[3]);

    for (LfgDungeonSet::const_iterator it = updateData.dungeons.begin(); it != updateData.dungeons.end(); ++it)
        data << uint32(*it);

    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[2]);
    data.WriteString(updateData.comment);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[4]);

    SendPacket(&data);
}

void WorldSession::SendLfgRoleChosen(uint64 guid, uint8 roles)
{
    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_ROLE_CHOSEN %s guid: %u roles: %u",
        GetPlayerInfo().c_str(), GUID_LOPART(guid), roles);

    WorldPacket data(SMSG_LFG_ROLE_CHOSEN, 8 + 1 + 4);
    data << uint64(guid);                                  // Guid
    data << uint8(roles > 0);                              // Ready
    data << uint32(roles);                                 // Roles
    SendPacket(&data);
}

void WorldSession::SendLfgRoleCheckUpdate(const LfgRoleCheck& roleCheck)
{
    LfgDungeonSet dungeons;
    if (roleCheck.rDungeonId)
        dungeons.insert(roleCheck.rDungeonId);
    else
        dungeons = roleCheck.dungeons;

    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_ROLE_CHECK_UPDATE %s", GetPlayerInfo().c_str());
    WorldPacket data(SMSG_LFG_ROLE_CHECK_UPDATE, 4 + 1 + 1 + dungeons.size() * 4 + 1 + roleCheck.roles.size() * (8 + 1 + 4 + 1));

    data << uint32(roleCheck.state);                       // Check result
    data << uint8(roleCheck.state == LFG_ROLECHECK_INITIALITING);
    data << uint8(dungeons.size());                        // Number of dungeons
    if (!dungeons.empty())
    {
        for (LfgDungeonSet::iterator it = dungeons.begin(); it != dungeons.end(); ++it)
        {
            LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(*it);
            data << uint32(dungeon ? dungeon->Entry() : 0); // Dungeon
        }
    }

    data << uint8(roleCheck.roles.size());                 // Players in group
    if (!roleCheck.roles.empty())
    {
        // Leader info MUST be sent 1st :S
        uint64 guid = roleCheck.leader;
        uint8 roles = roleCheck.roles.find(guid)->second;
        data << uint64(guid);                              // Guid
        data << uint8(roles > 0);                          // Ready
        data << uint32(roles);                             // Roles
        Player* player = ObjectAccessor::FindPlayer(guid);
        data << uint8(player ? player->getLevel() : 0);    // Level

        for (LfgRolesMap::const_iterator it = roleCheck.roles.begin(); it != roleCheck.roles.end(); ++it)
        {
            if (it->first == roleCheck.leader)
                continue;

            guid = it->first;
            roles = it->second;
            data << uint64(guid);                          // Guid
            data << uint8(roles > 0);                      // Ready
            data << uint32(roles);                         // Roles
            player = ObjectAccessor::FindPlayer(guid);
            data << uint8(player ? player->getLevel() : 0);// Level
        }
    }
    SendPacket(&data);
}

void WorldSession::SendLfgJoinResult(LfgJoinResultData const& joinData)
{
    uint32 size = 0;
    ObjectGuid guid = GetPlayer()->GetGUID();
    uint32 queueId = sLFGMgr->GetQueueId(_player->GetGUID());
    for (LfgLockPartyMap::const_iterator it = joinData.lockmap.begin(); it != joinData.lockmap.end(); ++it)
        size += 8 + 4 + uint32(it->second.size()) * (4 + 4 + 4 + 4);

    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_JOIN_RESULT %s checkResult: %u checkValue: %u",
        GetPlayerInfo().c_str(), joinData.result, joinData.state);

    WorldPacket data(SMSG_LFG_JOIN_RESULT, 4 + 4 + size);

    data.WriteBit(guid[4]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[2]);
    data.WriteBits(joinData.lockmap.size(), 22);

    for (LfgLockPartyMap::const_iterator it = joinData.lockmap.begin(); it != joinData.lockmap.end(); ++it)
    {
        ObjectGuid playerGuid = it->first;
        data.WriteBit(playerGuid[4]);
        data.WriteBit(playerGuid[5]);
        data.WriteBit(playerGuid[6]);
        data.WriteBit(playerGuid[0]);
        data.WriteBits(it->second.size(), 20);
        data.WriteBit(playerGuid[7]);
        data.WriteBit(playerGuid[3]);
        data.WriteBit(playerGuid[2]);
        data.WriteBit(playerGuid[1]);
    }

    data.WriteBit(guid[1]);
    data.WriteBit(guid[3]);
    data.WriteBit(guid[6]);
    data.WriteBit(guid[7]);
    data.FlushBits();

    for (LfgLockPartyMap::const_iterator it = joinData.lockmap.begin(); it != joinData.lockmap.end(); ++it)
    {
        ObjectGuid playerGuid = it->first;
        for (LfgLockMap::const_iterator itr = it->second.begin(); itr != it->second.end(); ++itr)
        {
            data << uint32(itr->second);                       // Lock status
            data << uint32(0);                                 // Current itemLevel
            data << uint32(0);                                 // Required itemLevel
            data << uint32(itr->first);                        // Dungeon entry (id + type)
        }

        data.WriteByteSeq(playerGuid[0]);
        data.WriteByteSeq(playerGuid[1]);
        data.WriteByteSeq(playerGuid[2]);
        data.WriteByteSeq(playerGuid[5]);
        data.WriteByteSeq(playerGuid[3]);
        data.WriteByteSeq(playerGuid[6]);
        data.WriteByteSeq(playerGuid[4]);
        data.WriteByteSeq(playerGuid[7]);
    }

    data << uint8(joinData.state);                         // Check Value
    data << uint8(joinData.result);                        // Check Result

    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[5]);
    data << uint32(3);
    data << uint32(time(NULL));                            // Join date
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[3]);
    data << uint32(queueId);                               // Queue Id
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[6]);

    SendPacket(&data);
}

void WorldSession::SendLfgQueueStatus(LfgQueueStatusData const& queueData)
{
    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_QUEUE_STATUS %s dungeon: %u, waitTime: %d, "
        "avgWaitTime: %d, waitTimeTanks: %d, waitTimeHealer: %d, waitTimeDps: %d, "
        "queuedTime: %u, tanks: %u, healers: %u, dps: %u",
        GetPlayerInfo().c_str(), queueData.dungeonId, queueData.waitTime, queueData.waitTimeAvg,
        queueData.waitTimeTank, queueData.waitTimeHealer, queueData.waitTimeDps,
        queueData.queuedTime, queueData.tanks, queueData.healers, queueData.dps);

    WorldPacket data(SMSG_LFG_QUEUE_STATUS, 4 + 4 + 4 + 4 + 4 +4 + 1 + 1 + 1 + 4);
    data << uint32(queueData.dungeonId);                   // Dungeon
    data << int32(queueData.waitTimeAvg);                  // Average Wait time
    data << int32(queueData.waitTime);                     // Wait Time
    data << int32(queueData.waitTimeTank);                 // Wait Tanks
    data << int32(queueData.waitTimeHealer);               // Wait Healers
    data << int32(queueData.waitTimeDps);                  // Wait Dps
    data << uint8(queueData.tanks);                        // Tanks needed
    data << uint8(queueData.healers);                      // Healers needed
    data << uint8(queueData.dps);                          // Dps needed
    data << uint32(queueData.queuedTime);                  // Player wait time in queue
    SendPacket(&data);
}

void WorldSession::SendLfgPlayerReward(LfgPlayerRewardData const& rewardData)
{
    if (!rewardData.rdungeonEntry || !rewardData.sdungeonEntry || !rewardData.quest)
        return;

    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_PLAYER_REWARD %s rdungeonEntry: %u, sdungeonEntry: %u, done: %u",
        GetPlayerInfo().c_str(), rewardData.rdungeonEntry, rewardData.sdungeonEntry, rewardData.done);

    uint8 itemNum = rewardData.quest->GetRewItemsCount();

    WorldPacket data(SMSG_LFG_PLAYER_REWARD, 4 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 1 + itemNum * (4 + 4 + 4));
    data << uint32(rewardData.rdungeonEntry);              // Random Dungeon Finished
    data << uint32(rewardData.sdungeonEntry);              // Dungeon Finished
    data << uint8(rewardData.done);
    data << uint32(1);
    data << uint32(rewardData.quest->GetRewOrReqMoney());
    data << uint32(rewardData.quest->XPValue(GetPlayer()));
    data << uint32(0);
    data << uint32(0);
    data << uint8(itemNum);
    if (itemNum)
    {
        for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
            if (uint32 itemId = rewardData.quest->RewardItemId[i])
            {
                ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
                data << uint32(itemId);
                data << uint32(item ? item->DisplayInfoID : 0);
                data << uint32(rewardData.quest->RewardItemIdCount[i]);
            }
    }
    SendPacket(&data);
}

void WorldSession::SendLfgBootProposalUpdate(LfgPlayerBoot const& boot)
{
    uint64 guid = GetPlayer()->GetGUID();
    LfgAnswer playerVote = boot.votes.find(guid)->second;
    uint8 votesNum = 0;
    uint8 agreeNum = 0;
    uint32 secsleft = uint8((boot.cancelTime - time(NULL)) / 1000);
    for (LfgAnswerContainer::const_iterator it = boot.votes.begin(); it != boot.votes.end(); ++it)
    {
        if (it->second != LFG_ANSWER_PENDING)
        {
            ++votesNum;
            if (it->second == LFG_ANSWER_AGREE)
                ++agreeNum;
        }
    }
    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_BOOT_PROPOSAL_UPDATE %s inProgress: %u - "
        "didVote: %u - agree: %u - victim: %u votes: %u - agrees: %u - left: %u - "
        "needed: %u - reason %s",
        GetPlayerInfo().c_str(), uint8(boot.inProgress), uint8(playerVote != LFG_ANSWER_PENDING),
        uint8(playerVote == LFG_ANSWER_AGREE), GUID_LOPART(boot.victim), votesNum, agreeNum,
        secsleft, LFG_GROUP_KICK_VOTES_NEEDED, boot.reason.c_str());
    WorldPacket data(SMSG_LFG_BOOT_PROPOSAL_UPDATE, 1 + 1 + 1 + 1 + 8 + 4 + 4 + 4 + 4 + boot.reason.length());
    data << uint8(boot.inProgress);                        // Vote in progress
    data << uint8(playerVote != LFG_ANSWER_PENDING);       // Did Vote
    data << uint8(playerVote == LFG_ANSWER_AGREE);         // Agree
    data << uint8(0);                                      // Unknown 4.2.2
    data << uint64(boot.victim);                           // Victim GUID
    data << uint32(votesNum);                              // Total Votes
    data << uint32(agreeNum);                              // Agree Count
    data << uint32(secsleft);                              // Time Left
    data << uint32(LFG_GROUP_KICK_VOTES_NEEDED);           // Needed Votes
    data << boot.reason.c_str();                           // Kick reason
    SendPacket(&data);
}

void WorldSession::SendLfgUpdateProposal(LfgProposal const& proposal)
{
    uint64 guid = GetPlayer()->GetGUID();
    uint64 gguid = proposal.players.find(guid)->second.group;
    bool silent = !proposal.isNew && gguid == proposal.group;
    uint32 dungeonEntry = proposal.dungeonId;
    uint32 queueId = sLFGMgr->GetQueueId(_player->GetGUID());
    time_t joinTime = sLFGMgr->GetQueueJoinTime(_player->GetGUID());

    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_PROPOSAL_UPDATE %s state: %u",
        GetPlayerInfo().c_str(), proposal.state);

    // show random dungeon if player selected random dungeon and it's not lfg group
    if (!silent)
    {
        LfgDungeonSet const& playerDungeons = sLFGMgr->GetSelectedDungeons(guid);
        if (playerDungeons.find(proposal.dungeonId) == playerDungeons.end())
            dungeonEntry = (*playerDungeons.begin());
    }

    dungeonEntry = sLFGMgr->GetLFGDungeonEntry(dungeonEntry);

    WorldPacket data(SMSG_LFG_PROPOSAL_UPDATE, 4 + 1 + 4 + 4 + 1 + 1 + proposal.players.size() * (4 + 1 + 1 + 1 + 1 +1));
    ObjectGuid l_PlayerGuid = guid;
    ObjectGuid l_GroupGuid = gguid;

    data.WriteBit(l_PlayerGuid[1]);
    data.WriteBit(silent);
    data.WriteBit(l_GroupGuid[0]);
    data.WriteBit(l_GroupGuid[1]);
    data.WriteBit(l_PlayerGuid[4]);
    data.WriteBit(l_GroupGuid[7]);
    data.WriteBit(l_PlayerGuid[2]);
    data.WriteBit(l_GroupGuid[3]);
    data.WriteBit(l_GroupGuid[5]);
    data.WriteBits(proposal.players.size(), 21);
    data.WriteBit(l_GroupGuid[4]);
    data.WriteBit(1);
    data.WriteBit(l_PlayerGuid[6]);

    for (LfgProposalPlayerContainer::const_iterator it = proposal.players.begin(); it != proposal.players.end(); ++it)
    {
        LfgProposalPlayer const& player = it->second;

        data.WriteBit(player.group ? player.group == proposal.group : 0);      // Is group in dungeon
        data.WriteBit(player.accept == LFG_ANSWER_AGREE);
        data.WriteBit(it->first == guid);
        data.WriteBit(player.accept != LFG_ANSWER_PENDING);
        data.WriteBit(player.group ? player.group == gguid : 0);               // Same group as the player
    }

    data.WriteBit(l_PlayerGuid[5]);
    data.WriteBit(l_PlayerGuid[7]);
    data.WriteBit(l_PlayerGuid[3]);
    data.WriteBit(l_GroupGuid[2]);
    data.WriteBit(l_GroupGuid[6]);
    data.WriteBit(l_PlayerGuid[0]);
    data.FlushBits();

    data.WriteByteSeq(l_GroupGuid[5]);
    data.WriteByteSeq(l_GroupGuid[1]);
    data.WriteByteSeq(l_PlayerGuid[5]);
    data << uint32(proposal.id);                           // Proposal Id
    data.WriteByteSeq(l_GroupGuid[2]);
    data.WriteByteSeq(l_GroupGuid[3]);
    data.WriteByteSeq(l_GroupGuid[7]);
    data << uint8(proposal.state);                         // State
    data << uint32(dungeonEntry);                          // Dungeon
    data.WriteByteSeq(l_GroupGuid[4]);
    data.WriteByteSeq(l_GroupGuid[6]);
    data << uint32(proposal.encounters);                   // Encounters done
    data.WriteByteSeq(l_PlayerGuid[4]);
    data.WriteByteSeq(l_PlayerGuid[3]);
    data.WriteByteSeq(l_PlayerGuid[0]);
    data.WriteByteSeq(l_PlayerGuid[2]);
    data.WriteByteSeq(l_PlayerGuid[6]);
    data << uint32(joinTime);
    data.WriteByteSeq(l_PlayerGuid[7]);

    for (LfgProposalPlayerContainer::const_iterator it = proposal.players.begin(); it != proposal.players.end(); ++it)
    {
        LfgProposalPlayer const& player = it->second;
        data << uint32(player.role);
    }

    data.WriteByteSeq(l_PlayerGuid[1]);
    data << uint32(3);                                     // Always 3
    data.WriteByteSeq(l_GroupGuid[0]);
    data << uint32(queueId);                               // Queue Id

    SendPacket(&data);
}

void WorldSession::SendLfgLfrList(bool update)
{
    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_LFR_LIST %s update: %u",
        GetPlayerInfo().c_str(), update ? 1 : 0);
    WorldPacket data(SMSG_LFG_UPDATE_SEARCH, 1);
    data << uint8(update);                                 // In Lfg Queue?
    SendPacket(&data);
}

void WorldSession::SendLfgDisabled()
{
    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_DISABLED %s", GetPlayerInfo().c_str());
    WorldPacket data(SMSG_LFG_DISABLED, 0);
    SendPacket(&data);
}

void WorldSession::SendLfgOfferContinue(uint32 dungeonEntry)
{
    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_OFFER_CONTINUE %s dungeon entry: %u",
        GetPlayerInfo().c_str(), dungeonEntry);
    WorldPacket data(SMSG_LFG_OFFER_CONTINUE, 4);
    data << uint32(dungeonEntry);
    SendPacket(&data);
}

void WorldSession::SendLfgTeleportError(uint8 err)
{
    sLog->outDebug(LOG_FILTER_LFG, "SMSG_LFG_TELEPORT_DENIED %s reason: %u",
        GetPlayerInfo().c_str(), err);
    WorldPacket data(SMSG_LFG_TELEPORT_DENIED, 4);
    data << uint32(err);                                   // Error
    SendPacket(&data);
}

void WorldSession::HandleDungeonFinderGetSystemInfo(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO,"CMSG_DUNGEON_FINDER_GET_SYSTEM_INFO [" UI64FMTD "]", GetPlayer()->GetGUID());
    bool unk1 = recvData.ReadBit();

    recvData.hexlike();
}

/*
void WorldSession::SendLfrUpdateListOpcode(uint32 dungeonEntry)
{
    sLog->outDebug(LOG_FILTER_PACKETIO, "SMSG_LFG_UPDATE_LIST %s dungeon entry: %u",
        GetPlayerInfo().c_str(), dungeonEntry);
    WorldPacket data(SMSG_LFG_UPDATE_LIST);
    SendPacket(&data);
}
*/
