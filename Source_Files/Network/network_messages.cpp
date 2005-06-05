/*

NETWORK_MESSAGES.CPP

	Copyright (C) 2005 and beyond by Gregory Smith
	and the "Aleph One" developers.
 
	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	This license is contained in the file "COPYING",
	which is included with this source code; it is available online at
	http://www.gnu.org/licenses/gpl.html

*/

#if !defined(DISABLE_NETWORKING)

#include "cseries.h"
#include "AStream.h"
#include "network_messages.h"
#include "network_private.h"
#include "network_data_formats.h"

static void write_string(AOStream& outputStream, const char *s) {
  outputStream.write(const_cast<char *>(s), strlen(s) + 1);
}

static void write_pstring(AOStream& outputStream, const unsigned char *s) {
  char cs[256];
  pstrcpy((unsigned char *) cs, s);
  a1_p2cstr((unsigned char *) cs);
  write_string(outputStream, cs);
}

static void read_string(AIStream& inputStream, char *s, size_t length) {
  char c;
  size_t i = 0;
  inputStream >> (int8&) c;
  while (c != '\0' && i < length - 1) {
    s[i++] = c;
    inputStream >> (int8&) c;
  }
  s[i] = '\0';
}

static void read_pstring(AIStream& inputStream, unsigned char *s, size_t length) {
  unsigned char ps[256];
  read_string(inputStream, (char *) ps, length > 256 ? 256 : length);
  a1_c2pstr((char *) ps);
  pstrcpy(s, ps);
}

// ghs: if you're trying to preserve network compatibility, and you want
//      to change a struct, you'll want to replace this function
//      with individualized versions in each message type, so that you can
//      add things to the end of the packet rather than in the middle

static void deflateNetPlayer(AOStream& outputStream, const NetPlayer &player) {

  outputStream.write((byte *) &player.dspAddress.host, 4);
  outputStream.write((byte *) &player.dspAddress.port, 2);
  outputStream.write((byte *) &player.ddpAddress.host, 4);
  outputStream.write((byte *) &player.ddpAddress.port, 2);
  outputStream << player.identifier;
  outputStream << player.stream_id;
  outputStream << (byte) player.net_dead;


  write_pstring(outputStream, player.player_data.name);
  outputStream << player.player_data.desired_color;
  outputStream << player.player_data.team;
  outputStream << player.player_data.color;
  outputStream.write(player.player_data.long_serial_number, 
		     sizeof(player.player_data.long_serial_number));
}

static void inflateNetPlayer(AIStream& inputStream, NetPlayer &player) {

  inputStream.read((byte *) &player.dspAddress.host, 4);
  inputStream.read((byte *) &player.dspAddress.port, 2);
  inputStream.read((byte *) &player.ddpAddress.host, 4);
  inputStream.read((byte *) &player.ddpAddress.port, 2);
  inputStream >> player.identifier;
  inputStream >> player.stream_id;
  inputStream >> (byte&) player.net_dead;

  read_pstring(inputStream, player.player_data.name, sizeof(player.player_data.name));  
  inputStream >> player.player_data.desired_color;
  inputStream >> player.player_data.team;
  inputStream >> player.player_data.color;
  inputStream.read(player.player_data.long_serial_number, sizeof(player.player_data.long_serial_number));
}


void AcceptJoinMessage::reallyDeflateTo(AOStream& outputStream) const {
  outputStream << (Uint8) mAccepted;
  deflateNetPlayer(outputStream, mPlayer);
}

bool AcceptJoinMessage::reallyInflateFrom(AIStream& inputStream) {
  inputStream >> (Uint8&) mAccepted;
  inflateNetPlayer(inputStream, mPlayer);
  return true;
}

void ChangeColorsMessage::reallyDeflateTo(AOStream& outputStream) const {
  outputStream << mColor;
  outputStream << mTeam;
}

bool ChangeColorsMessage::reallyInflateFrom(AIStream& inputStream) {
  inputStream >> mColor;
  inputStream >> mTeam;

  return true;
}

void JoinerInfoMessage::reallyDeflateTo(AOStream& outputStream) const {
  outputStream << mInfo.network_version;
  outputStream << mInfo.stream_id;
  outputStream.write(mInfo.name, mInfo.name[0] + 1);
}

bool JoinerInfoMessage::reallyInflateFrom(AIStream& inputStream) {
  inputStream >> mInfo.network_version;
  inputStream >> mInfo.stream_id;
  inputStream.read(mInfo.name, 1);
  if (mInfo.name[0] > 0) {
    inputStream.read(mInfo.name + 1, mInfo.name[0]);
  }
  return true;
}

void NetworkChatMessage::reallyDeflateTo(AOStream& outputStream) const {
  outputStream << mSenderID;
  outputStream << (Uint16) kTargetPlayers;
  write_string(outputStream, mChatText);
}

bool NetworkChatMessage::reallyInflateFrom(AIStream& inputStream) {
  inputStream >> mSenderID;
  Uint16 target;
  inputStream >> target;
  read_string(inputStream, mChatText, CHAT_MESSAGE_SIZE);
  return true;
}

void TopologyMessage::reallyDeflateTo(AOStream& outputStream) const {

  outputStream << mTopology.tag;
  outputStream << mTopology.player_count;
  outputStream << mTopology.nextIdentifier;

  outputStream << mTopology.game_data.initial_random_seed;
  outputStream << mTopology.game_data.net_game_type;
  outputStream << mTopology.game_data.time_limit;
  outputStream << mTopology.game_data.kill_limit;
  outputStream << mTopology.game_data.game_options;
  outputStream << mTopology.game_data.difficulty_level;
  outputStream << (byte) mTopology.game_data.server_is_playing;
  outputStream << (byte) mTopology.game_data.allow_mic;
  outputStream << mTopology.game_data.cheat_flags;
  outputStream << mTopology.game_data.level_number;
  write_string(outputStream, mTopology.game_data.level_name);
  outputStream << mTopology.game_data.initial_updates_per_packet;
  outputStream << mTopology.game_data.initial_update_latency;

  for (int i = 0; i < MAXIMUM_NUMBER_OF_NETWORK_PLAYERS; i++) {
    deflateNetPlayer(outputStream, mTopology.players[i]);
  }
}

bool TopologyMessage::reallyInflateFrom(AIStream& inputStream) {
  inputStream >> mTopology.tag;
  inputStream >> mTopology.player_count;
  inputStream >> mTopology.nextIdentifier;

  inputStream >> mTopology.game_data.initial_random_seed;
  inputStream >> mTopology.game_data.net_game_type;
  inputStream >> mTopology.game_data.time_limit;
  inputStream >> mTopology.game_data.kill_limit;
  inputStream >> mTopology.game_data.game_options;
  inputStream >> mTopology.game_data.difficulty_level;
  inputStream >> (byte&) mTopology.game_data.server_is_playing;
  inputStream >> (byte&) mTopology.game_data.allow_mic;
  inputStream >> mTopology.game_data.cheat_flags;
  inputStream >> mTopology.game_data.level_number;
  read_string(inputStream, mTopology.game_data.level_name, MAX_LEVEL_NAME_LENGTH - 1);
  inputStream >> mTopology.game_data.initial_updates_per_packet;
  inputStream >> mTopology.game_data.initial_update_latency;

  for (int i = 0; i < MAXIMUM_NUMBER_OF_NETWORK_PLAYERS; i++) {
    inflateNetPlayer(inputStream, mTopology.players[i]);
  }

  return true;
}

#endif // !defined(DISABLE_NETWORKING)

