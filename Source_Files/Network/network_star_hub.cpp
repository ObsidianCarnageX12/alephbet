/*
 *  network_star_hub.cpp

	Copyright (C) 2003 and beyond by Woody Zenfell, III
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

 *  The portion of the star game network protocol run on a single machine, perhaps (eventually)
 *	without an actual player on the machine - or even without most of the game, for that matter.
 *
 *  Created by Woody Zenfell, III on Mon Apr 28 2003.
 *
 *  May 27, 2003 (Woody Zenfell): lossy byte-stream distribution.
 *
 *  June 14, 2003 (Woody Zenfell):
 *	+ send_period preference allows hub to reduce upstream requirements (sends fewer packets -> less data)
 *	+ hub preferences now are only emitted if they differ from the defaults (easier to change defaults in future)
 *
 *  July 3, 2003 (Woody Zenfell):
 *	Lossy distribution should be more robust now (using a queue rather than holding a single element).
 *	Should help netmic all-around, but especially (I hope) on Windows.
 *
 *  July 9, 2003 (Woody Zenfell):
 *	Preliminary support for standalone hub (define A1_NETWORK_STANDALONE_HUB).
 *
 *  September 17, 2004 (jkvw):
 *	NAT-friendly networking - we no longer get spoke addresses form topology -
 *	instead spokes send identification packets to hub with player ID.
 *	Hub can then associate the ID in the identification packet with the paket's source address.
 */

#if !defined(DISABLE_NETWORKING)

#include "network_star.h"

//#include "sdl_network.h"
#include "TickBasedCircularQueue.h"
#include "network_private.h"
#include "mytm.h"
#include "AStream.h"
#include "Logging.h"
#include "WindowedNthElementFinder.h"
#include "CircularByteBuffer.h"
#include "XML_ElementParser.h"
#include "SDL_timer.h" // SDL_Delay()

#include <vector>
#include <map>
#include <algorithm> // std::min()

#include "crc.h"
#include "player.h" // for masking out action flags triggers :(

#define BANDWIDTH_REDUCTION 1

// Synchronization:
// hub_received_network_packet() is not reentrant
// hub_tick() is not reentrant
// hub_tick() and hub_received_network_packet() are mutex
// hmm had probably better take the lock in hub_cleanup() too as part of thread-cancellation strategy,
// unless thread-cancellation API guarantees not only that routine won't be called again, but that it's
// now finished executing as well.
// hub_initialize() should be safe as long as it's not called while the other threads are running.
// (this last bit assumes that hub_cleanup() doesn't exit until threads are completely finished.)

// specifically,
// In Mac OS 9, hrm hope that starting and stopping the packet listener and ticker
//   is safe with regard to hub_initialize() and hub_cleanup()
// In Mac OS X "Carbon", well maybe it should use full SDL-style stuff, sounds like Mac OS X
//   emulates Time Manager basically the same way I do anyway.  Well, if not moving to full
//   SDL-style, need to take mytm_mutex inside hub_tick, hub_initialize,
//   and hub_cleanup.  (mytm_mutex is already taken by packet-listener emulation.)
// In full SDL-style, mytm_mutex is already taken by both packet-listener and mytm emulation,
//   so we just need to take it at hub_initialize and hub_cleanup.
// All of the above applies to spokes as well.

// Hub<->local spoke communication: (THIS APPLIES ONLY TO THE LOCAL SPOKE ON THE HUB'S MACHINE)
// Since current NetDDP* system only supports the notion of a single datagram socket,
//   we need to have another mechanism to allow the local spoke and the hub to communicate.
// I propose having one packet-buffer for each direction of communication, into which the
//   "outgoing" packet is stored.  When the task that generated the packet is finished, instead
//   of returning and allowing the mutex to be released, it calls the other task directly.
//   Naturally this other task must not take the mutex, because we're already holding it.
//   Note that this other task could potentially generate a packet also.
// So, here are the "longest" possible situations:
//   TAKE MUTEX - hub_tick stores packet - spoke_received_packet - RELEASE MUTEX
//   TAKE MUTEX - hub_received_packet stores packet - spoke_received_packet - RELEASE MUTEX
//   TAKE MUTEX - spoke_tick stores packet - hub_received_packet stores packet - spoke_received_packet - RELEASE MUTEX
// Hmm, spoke_received_packet won't ever be called outside of the above circumstances, since there's
//   no real socket it can receive packets on.
// All of the above "chains" are easily accomplished, and fall out naturally from whatever mechanisms
//   are in place for synchronization as listed further above.

// What do we need to know, initially, from the outside world?
// number of players
// address for each player - jkvw: We no longer know this, since we want spokes to operate behind firewalls,
//				   and their packets may be subject to port remapping.  We do have player IDs though.
// whether each player is actually connected
// starting game-tick
// local player index (only for data received reference tick and for hacky communicate-with-local-spoke stuff)

/*
enum NetworkState {
        eNetworkDown,
        eNetworkStartingUp,
        eNetworkUp,
        eNetworkComingDown
};

static NetworkState sNetworkState;
*/


enum {
	kFlagsQueueSize = TICKS_PER_SECOND * 5,
        kDefaultPregameWindowSize = TICKS_PER_SECOND / 2,
        kDefaultInGameWindowSize = TICKS_PER_SECOND * 5,
	kDefaultPregameNthElement = 2,
	kDefaultInGameNthElement = kDefaultInGameWindowSize / 2,
        kDefaultPregameTicksBeforeNetDeath = 20 * TICKS_PER_SECOND,
        kDefaultInGameTicksBeforeNetDeath = 3 * TICKS_PER_SECOND,
	kDefaultSendPeriod = 1,
        kDefaultRecoverySendPeriod = TICKS_PER_SECOND / 2,
	kDefaultMinimumSendPeriod = 5,
	kLossyByteStreamDataBufferSize = 1280,
	kTypicalLossyByteStreamChunkSize = 56,
	kLossyByteStreamDescriptorCount = kLossyByteStreamDataBufferSize / kTypicalLossyByteStreamChunkSize	
};


struct HubPreferences {
//	int32	mFlagsQueueSize;
	int32	mPregameWindowSize;
	int32	mInGameWindowSize;
	int32	mPregameNthElement;
	int32	mInGameNthElement;
	int32	mPregameTicksBeforeNetDeath;
	int32	mInGameTicksBeforeNetDeath;
	int32	mSendPeriod;
	int32	mRecoverySendPeriod;
	int32   mMinimumSendPeriod;
};

static HubPreferences sHubPreferences;

int32& hub_get_minimum_send_period() { return sHubPreferences.mMinimumSendPeriod; }

// sNetworkTicker advances even if the game clock doesn't.
// sLastNetworkTickSent is used to force us to resend packets (at a lower rate) even if we're no longer
// getting new data.
static int32 sNetworkTicker;
static int32 sLastNetworkTickSent;

// We have a pregame startup period to help establish (via standard adjustment mechanism) everyone's
// timing.  Ticks smaller than sSmallestRealGameTick are part of this startup period.  They smell
// just like real in-game ticks, except that spokes won't enqueue them on player_queues, and we
// may have different adjustment window sizes and timeout periods for pre-game and in-game ticks.
static int32 sSmallestRealGameTick;

// Once everyone ACKs this tick, we're satisfied the game is ended.  (They should all agree on which
// tick is last due to the symmetric execution model.)
static int32 sSmallestPostGameTick;

// The sFlagsQueues hold all flags for ticks for which we've received data from at least one
// station, but for which we haven't received an ACK from all stations.
// sFlagsQueues[all].getReadIndex() == sPlayerDataDisposition.getReadIndex();
// max(sFlagsQueues[all].getWriteIndex()) == sPlayerDataDisposition.getWriteIndex();
// min(sFlagsQueues[all].getWriteIndex()) == sSmallestIncompleteTick;
typedef std::vector<TickBasedActionQueue> TickBasedActionQueueCollection;
static TickBasedActionQueueCollection	sFlagsQueues;

// tracks the net ticks each flags tick was *first* sent out at
static ConcreteTickBasedCircularQueue<int32> sFlagSendTimeQueue(kFlagsQueueSize);
static int32 sLastRealUpdate;

struct NetworkPlayer_hub {
        NetAddrBlock	mAddress;		// network address of player
	bool		mAddressKnown;		// did player tell us his address yet?
        bool		mConnected;		// is player still connected?
        int32		mLastNetworkTickHeard;	// our sNetworkTicker last time we got a packet from them
        int32		mSmallestUnacknowledgedTick;

	WindowedNthElementFinder<int32>	mNthElementFinder;
	// We can "hear" ticks that we can't enqueue - this is useful for timing data.
	int32		mSmallestUnheardTick;

        // When we decide a timing adjustment is needed, we include the timing adjustment
        // request in every packet outbound to the player until we're sure he's seen it.
        // In particular, mTimingAdjustmentTick is set to the sSmallestIncompleteTick, so we
        // know nobody's received data for that tick yet.  We continue to send the message
        // until the station ACKs past that tick; at that point we know he must have seen
        // our message.
        // When we get that ACK, we clear the window and let it fill up again to make sure
        // we have clean, fresh, post-adjustment data to work from.  We won't make any new
        // timing adjustment requests to a station with an outstanding timing adjustment
        // or an incomplete averaging window.
        int		mOutstandingTimingAdjustment;
        int32		mTimingAdjustmentTick;

        // If the player is dropped during a game, we need to tell the other players about it.
        // This is accomplished in much the same way as the timing adjustment - we include
        // a "player netdead" message in every outbound packet until we're sure they've gotten
        // the message.  mNetDeadTick, then, works pretty much just like mTimingAdjustmentTick.
        // mNetDeadTick is the first tick for which the netdead player isn't providing data.
        int32		mNetDeadTick;

	// the last time a recovery set of flags was sent instead of incremental
	int32           mLastRecoverySend;

	// latency stuff
	vector<int32> mDisplayLatencyBuffer; // last 30 latency calculations in ticks
	uint32 mDisplayLatencyCount;
	int32 mDisplayLatencyTicks; // sum of the latency ticks from the last second, using above two
};

// Housekeeping queues:
// sPlayerDataDisposition holds an element for every tick for which data has been received from
// someone, but which at least one player has not yet acknowledged.
// sPlayerDataDisposition.getReadIndex() <= sSmallestIncompleteTick <= sPlayerDataDisposition.getWriteIndex()
// sSmallestIncompleteTick indexes into sPlayerDataDisposition also; it divides the queue into ticks
// for which data has been received from someone but not yet everyone (>= sSmallestIncompleteTick) and
// ticks for which data has been sent out (to everyone) but for which someone hasn't yet acknowledged
// (< sSmallestIncompleteTick).

// The value of a queue element is a bit-set (indexed by player index) with a 1 bit for each player
// that we're waiting on.  So, we can mask out successive players' bits as their traffic reaches us;
// when the value hits 0, all players have checked in and we can advance an index.
// sConnectedPlayersBitmask has '1' set for every connected player.
static MutableElementsTickBasedCircularQueue<uint32>	sPlayerDataDisposition(kFlagsQueueSize);
static int32 sSmallestIncompleteTick;
static uint32 sConnectedPlayersBitmask;
static uint32 sLaggingPlayersBitmask;


// sPlayerReflectedFlags holds an element for every tick for which data has been
// sent but at least one player has not yet acknowledged
//
// the value of a queue element is a bit-set (indexed by player index) with a 1
// bit for each player we've altered flags and need to reflect flags for
static MutableElementsTickBasedCircularQueue<uint32> sPlayerReflectedFlags(kFlagsQueueSize);

// holds the last real flags we received from this player
static vector<action_flags_t> sLastFlagsReceived;

// sSmallestUnsentTick is used for reducing the number of packets sent: we won't send a packet unless
// sSmallestIncompleteTick - sSmallestUnsentTick >= sHubPreferences.mSendPeriod
static int32 sSmallestUnsentTick;

struct NetAddrBlockCompare
{

  bool operator()(const NetAddrBlock& a, const NetAddrBlock& b) const
  {
    if (a.host == b.host)
      return a.port < b.port;
    else
      return a.host < b.host;
  }
};

  typedef std::map<NetAddrBlock, int, NetAddrBlockCompare>	AddressToPlayerIndexType;
static AddressToPlayerIndexType	sAddressToPlayerIndex;

typedef std::vector<NetworkPlayer_hub>	NetworkPlayerCollection;
static NetworkPlayerCollection	sNetworkPlayers;

// Local player index is used to decide how to send a packet; ref is used for timing.
static size_t			sLocalPlayerIndex;
static size_t			sReferencePlayerIndex;

static DDPFramePtr	sOutgoingFrame = NULL;

#ifndef A1_NETWORK_STANDALONE_HUB
static DDPPacketBuffer	sLocalOutgoingBuffer;
static bool		sNeedToSendLocalOutgoingBuffer = false;
#endif


struct HubLossyByteStreamChunkDescriptor
{
	uint16	mLength;
	int16	mType;
	uint32	mDestinations;
	uint8	mSender;
};

// This holds outgoing lossy byte stream data
static CircularByteBuffer sOutgoingLossyByteStreamData(kLossyByteStreamDataBufferSize);

// This holds a descriptor for each chunk of lossy byte stream data held in the above buffer
static CircularQueue<HubLossyByteStreamChunkDescriptor> sOutgoingLossyByteStreamDescriptors(kLossyByteStreamDescriptorCount);

// This is used to copy between AStream and CircularByteBuffer
// It's used in both directions, but that's ok because the routines that do so are mutex.
static byte sScratchBuffer[kLossyByteStreamDataBufferSize];


static myTMTaskPtr	sHubTickTask = NULL;
static bool		sHubActive = false;	// used to enable the packet handler
static bool		sHubInitialized = false;



static void hub_check_for_completion();
static void player_acknowledged_up_to_tick(size_t inPlayerIndex, int32 inSmallestUnacknowledgedTick);
static bool player_provided_flags_from_tick_to_tick(size_t inPlayerIndex, int32 inFirstNewTick, int32 inSmallestUnreceivedTick);
static void hub_received_game_data_packet_v1(AIStream& ps, int inSenderIndex);
static void hub_received_identification_packet(AIStream& ps, NetAddrBlock address);
static void process_messages(AIStream& ps, int inSenderIndex);
static void process_optional_message(AIStream& ps, int inSenderIndex, uint16 inMessageType);
static void make_player_netdead(int inPlayerIndex);
static bool hub_tick();
static void send_packets();



// These are excellent candidates for templatization, but MSVC++6.0 has broken function templates.
// (Actually, they might not be broken if the template parameter is a typename, but... not taking chances.)
static inline NetworkPlayer_hub&
getNetworkPlayer(size_t inIndex)
{
        assert(inIndex < sNetworkPlayers.size());
        return sNetworkPlayers[inIndex];
}

static inline TickBasedActionQueue&
getFlagsQueue(size_t inIndex)
{
        assert(inIndex < sFlagsQueues.size());
        return sFlagsQueues[inIndex];
}



static inline bool
operator <(const NetAddrBlock& a, const NetAddrBlock& b)
{
        return memcmp(&a, &b, sizeof(a)) < 0;
}


static OSErr
send_frame_to_local_spoke(DDPFramePtr frame, NetAddrBlock *address, short protocolType, short port)
{
#ifndef A1_NETWORK_STANDALONE_HUB
        sLocalOutgoingBuffer.datagramSize = frame->data_size;
        memcpy(sLocalOutgoingBuffer.datagramData, frame->data, frame->data_size);
        sLocalOutgoingBuffer.protocolType = protocolType;
        // We ignore the 'source address' because the spoke does too.
        sNeedToSendLocalOutgoingBuffer = true;
        return noErr;
#else
	// Standalone hub should never call this routine
	assert(false);
	return noErr;
#endif // A1_NETWORK_STANDALONE_HUB
}



static inline void
check_send_packet_to_spoke()
{
	// Routine exists but has no implementation on standalone hub.
#ifndef A1_NETWORK_STANDALONE_HUB
        if(sNeedToSendLocalOutgoingBuffer)
                spoke_received_network_packet(&sLocalOutgoingBuffer);

        sNeedToSendLocalOutgoingBuffer = false;
#endif // A1_NETWORK_STANDALONE_HUB
}



#ifndef INT32_MAX
#define INT32_MAX 0x7fffffff
#endif

void
hub_initialize(int32 inStartingTick, size_t inNumPlayers, const NetAddrBlock* const* inPlayerAddresses, size_t inLocalPlayerIndex)
{
//        assert(sNetworkState == eNetworkDown);

//        sNetworkState = eNetworkJustStartingUp;

        assert(inLocalPlayerIndex < inNumPlayers);
        sLocalPlayerIndex = inLocalPlayerIndex;
	sReferencePlayerIndex = sLocalPlayerIndex;

#ifdef A1_NETWORK_STANDALONE_HUB
	// There is no local player on standalone hub.
	sLocalPlayerIndex = (size_t)NONE;
#endif

	sSmallestPostGameTick = INT32_MAX;
        sSmallestRealGameTick = inStartingTick;
        int32 theFirstTick = inStartingTick - kPregameTicks;

        if(sOutgoingFrame == NULL)
                sOutgoingFrame = NetDDPNewFrame();

#ifndef A1_NETWORK_STANDALONE_HUB
        sNeedToSendLocalOutgoingBuffer = false;
#endif

        sNetworkPlayers.clear();
        sFlagsQueues.clear();
        sNetworkPlayers.resize(inNumPlayers);
        sFlagsQueues.resize(inNumPlayers, TickBasedActionQueue(kFlagsQueueSize));
        sAddressToPlayerIndex.clear();
        sConnectedPlayersBitmask = 0;

	sOutgoingLossyByteStreamDescriptors.reset();
	sOutgoingLossyByteStreamData.reset();

        for(size_t i = 0; i < inNumPlayers; i++)
        {
                NetworkPlayer_hub& thePlayer = sNetworkPlayers[i];

                if(inPlayerAddresses[i] != NULL)
                {
                        thePlayer.mConnected = true;
                        sConnectedPlayersBitmask |= (((uint32)1) << i);
			thePlayer.mAddressKnown = false;
                        // thePlayer.mAddress = *(inPlayerAddresses[i]); (jkvw: see note below)
                        // Currently, all-0 address is cue for local spoke.
			// jkvw: The "real" addresses for spokes won't be known unti we get some UDP traffic
			//	 from them - we'll update as they become known.
                        if(i == sLocalPlayerIndex) { // jkvw: I don't need this, do I?
                                obj_clear(thePlayer.mAddress);
				sAddressToPlayerIndex[thePlayer.mAddress] = i;
				thePlayer.mAddressKnown = true;
			}
                }
                else
                {
                        thePlayer.mConnected = false;
                }

                thePlayer.mLastNetworkTickHeard = 0;
		thePlayer.mLastRecoverySend = theFirstTick;
                thePlayer.mSmallestUnacknowledgedTick = theFirstTick;
		thePlayer.mSmallestUnheardTick = theFirstTick;
		thePlayer.mNthElementFinder.reset(sHubPreferences.mPregameWindowSize);
                thePlayer.mOutstandingTimingAdjustment = 0;
                thePlayer.mTimingAdjustmentTick = 0;
                thePlayer.mNetDeadTick = theFirstTick - 1;

		thePlayer.mDisplayLatencyBuffer.resize(TICKS_PER_SECOND);
		thePlayer.mDisplayLatencyCount = 0;
		thePlayer.mDisplayLatencyTicks = 0;

                sFlagsQueues[i].reset(theFirstTick);
        }
        
        sPlayerDataDisposition.reset(theFirstTick);
	sPlayerReflectedFlags.reset(theFirstTick);
	sLastFlagsReceived.resize(inNumPlayers);
	sFlagSendTimeQueue.reset(theFirstTick);
        sSmallestIncompleteTick = theFirstTick;
	sSmallestUnsentTick = theFirstTick;
        sNetworkTicker = 0;
        sLastNetworkTickSent = 0;
	sLastRealUpdate = 0;
	sLaggingPlayersBitmask = 0;

        sHubActive = true;

        sHubTickTask = myXTMSetup(1000/TICKS_PER_SECOND, hub_tick);

	sHubInitialized = true;
}



void
hub_cleanup(bool inGraceful, int32 inSmallestPostGameTick)
{
	if(sHubInitialized)
	{
		if(inGraceful)
		{
			// Signal our demise
			sSmallestPostGameTick = inSmallestPostGameTick;

			// We have to do a check now in case the conditions are already met
			if(take_mytm_mutex())
			{
				hub_check_for_completion();
				release_mytm_mutex();
			}
			
			// Now we should wait/sleep for the rest of the machinery to wind down
			// Packet handler will set sHubActive = false once it has acks from all connected players;
			while(sHubActive)
			{
// Here we try to isolate the "Classic" Mac OS (we can only sleep on the others)
#if !defined(mac) || defined(__MACH__)
				SDL_Delay(10);
#endif
			}
		}
		else
		{		
			// Stop processing incoming packets (packet processor won't start processing another packet
			// due to sHubActive = false, and we know it's not in the middle of processing one because
			// we take the mutex).
			if(take_mytm_mutex())
			{
				sHubActive = false;
				release_mytm_mutex();
			}
		}
	
		// Mark the tick task for cancellation (it won't start running again after this returns).
		myTMRemove(sHubTickTask);
		sHubTickTask = NULL;
	
		// This waits for the tick task to actually finish - so we know the tick task isn't in
		// the middle of processing when we do the rest of the cleanup below.
		myTMCleanup(true);
		
		sNetworkPlayers.clear();
		sFlagsQueues.clear();

		sAddressToPlayerIndex.clear();
		NetDDPDisposeFrame(sOutgoingFrame);
		sOutgoingFrame = NULL;
	}
}



static void
hub_check_for_completion()
{
	// When all players (including the local spoke) have either ACKed up to sSmallestPostGameTick
	// or become disconnected, we're clear to cleanup.  (In other words, we should avoid cleaning
	// up if there are connected players that haven't ACKed up to the game's end tick.)
	bool someoneStillActive = false;
	for(size_t i = 0; i < sNetworkPlayers.size(); i++)
	{
		NetworkPlayer_hub& thePlayer = sNetworkPlayers[i];
		if(thePlayer.mConnected && thePlayer.mSmallestUnacknowledgedTick < sSmallestPostGameTick)
		{
			someoneStillActive = true;
			break;
		}
	}

	if(!someoneStillActive)
		sHubActive = false;
}
		


void
hub_received_network_packet(DDPPacketBufferPtr inPacket)
{
        // Processing packets?
        if(!sHubActive)
                return;

	logContextNMT("hub processing a received packet");
	
        AIStreamBE ps(inPacket->datagramData, inPacket->datagramSize);

	try {
		uint16	thePacketMagic;
		ps >> thePacketMagic;

		uint16 thePacketCRC;
		ps >> thePacketCRC;

		if (thePacketCRC != calculate_data_crc_ccitt(&inPacket->datagramData[kStarPacketHeaderSize], inPacket->datagramSize - kStarPacketHeaderSize))
		{
			logWarningNMT1("CRC failure; discarding packet type %i", thePacketMagic);
		}
		
                switch(thePacketMagic)
                {
                        case kSpokeToHubGameDataPacketV1Magic:
			{
				// Find sender
				AddressToPlayerIndexType::iterator theEntry = sAddressToPlayerIndex.find(inPacket->sourceAddress);
				if(theEntry == sAddressToPlayerIndex.end())
					return;
				
				int theSenderIndex = theEntry->second;
				
				// Unconnected players should not have entries in sAddressToPlayerIndex
				assert(getNetworkPlayer(theSenderIndex).mConnected);
				hub_received_game_data_packet_v1(ps, theSenderIndex);
			}
			break;

			case kSpokeToHubIdentification:
				hub_received_identification_packet(ps, inPacket->sourceAddress);
			break;

                        default:
			break;
                }
	}
        catch (...)
	{
		// ignore errors - we just discard the packet, effectively.
	}

        check_send_packet_to_spoke();
}


static void
hub_received_identification_packet(AIStream& ps, NetAddrBlock address)
{
	int16 theSenderIndex;
	ps >> theSenderIndex;
	
	if (!sNetworkPlayers[theSenderIndex].mAddressKnown) {
		sAddressToPlayerIndex[address] = theSenderIndex;
		sNetworkPlayers[theSenderIndex].mAddressKnown = true;
		sNetworkPlayers[theSenderIndex].mAddress = address;
	}

} // hub_received_idetification_packet()


// I suppose to be safer, this should check the entire packet before acting on any of it.
// As it stands, a malformed packet could have have a well-formed prefix of it interpreted
// before the remainder is discarded.
static void
hub_received_game_data_packet_v1(AIStream& ps, int inSenderIndex)
{
        // Process the piggybacked acknowledgement
        int32	theSmallestUnacknowledgedTick;
        ps >> theSmallestUnacknowledgedTick;

        // If ack is too soon we throw out the entire packet to be safer
        if(theSmallestUnacknowledgedTick > sSmallestIncompleteTick)
        {
                logAnomalyNMT3("received ack from player %d for tick %d; have only sent up to %d", inSenderIndex, theSmallestUnacknowledgedTick, sSmallestIncompleteTick);
                return;
        }                

        player_acknowledged_up_to_tick(inSenderIndex, theSmallestUnacknowledgedTick);

        // If that's all the data, we're done
        if(ps.tellg() == ps.maxg())
                return;

        // Process messages, if present
        process_messages(ps, inSenderIndex);

        // If that's all the data, we're done
        if(ps.tellg() == ps.maxg())
                return;

        // If present, process the action_flags
        int32	theStartTick;
        ps >> theStartTick;

        // Make sure there's an integral number of action_flags
        int	theRemainingDataLength = ps.maxg() - ps.tellg();
        if(theRemainingDataLength % kActionFlagsSerializedLength != 0)
                return;

        int32	theActionFlagsCount = theRemainingDataLength / kActionFlagsSerializedLength;

        TickBasedActionQueue& theQueue = getFlagsQueue(inSenderIndex);

        // Packet is malformed if they're skipping ahead
        if(theStartTick > theQueue.getWriteTick())
                return;

        // Skip redundant flags without processing/checking them
        int	theRedundantActionFlagsCount = std::min(theQueue.getWriteTick() - theStartTick, theActionFlagsCount);
        int	theRedundantDataLength = theRedundantActionFlagsCount * kActionFlagsSerializedLength;
	ps.ignore(theRedundantDataLength);



        // Enqueue flags that are new to us
        int	theUsefulActionFlagsCount = theActionFlagsCount - theRedundantActionFlagsCount;
        int	theRemainingQueueSpace = (sPlayerDataDisposition.getReadTick() < sSmallestRealGameTick && theQueue.size() > sHubPreferences.mPregameWindowSize) ? 0 : theQueue.availableCapacity();
	
        int	theEnqueueableFlagsCount = std::min(theUsefulActionFlagsCount, theRemainingQueueSpace);
        
        for(int i = 0; i < theEnqueueableFlagsCount; i++)
        {
                action_flags_t theActionFlags;
                ps >> theActionFlags;
                theQueue.enqueue(theActionFlags);
		sLastFlagsReceived[inSenderIndex] = theActionFlags;
        }

	// Update timing data
	NetworkPlayer_hub& thePlayer = getNetworkPlayer(inSenderIndex);
	NetworkPlayer_hub& theReferencePlayer = getNetworkPlayer(sReferencePlayerIndex);
	while(thePlayer.mSmallestUnheardTick < theStartTick + theActionFlagsCount)
	{
		int32 theReferenceTick = theReferencePlayer.mSmallestUnheardTick;
		int32 theArrivalOffset = thePlayer.mSmallestUnheardTick - theReferenceTick;
		logDumpNMT2("player %d's arrivalOffset is %d", inSenderIndex, theArrivalOffset);
		thePlayer.mNthElementFinder.insert(theArrivalOffset);
		thePlayer.mSmallestUnheardTick++;
	}

        // Make the pregame -> ingame transition
        if(thePlayer.mSmallestUnheardTick >= sSmallestRealGameTick && static_cast<int32>(thePlayer.mNthElementFinder.window_size()) != sHubPreferences.mInGameWindowSize)
		thePlayer.mNthElementFinder.reset(sHubPreferences.mInGameWindowSize);

	if(thePlayer.mOutstandingTimingAdjustment == 0 && thePlayer.mNthElementFinder.window_full())
	{
		thePlayer.mOutstandingTimingAdjustment = thePlayer.mNthElementFinder.nth_smallest_element((thePlayer.mSmallestUnheardTick >= sSmallestRealGameTick) ? sHubPreferences.mInGameNthElement : sHubPreferences.mPregameNthElement);
		if(thePlayer.mOutstandingTimingAdjustment != 0)
		{
			thePlayer.mTimingAdjustmentTick = sSmallestIncompleteTick;
			logTraceNMT3("tick %d: asking player %d to adjust timing by %d", sSmallestIncompleteTick, inSenderIndex, thePlayer.mOutstandingTimingAdjustment);
		}
	}

        // Do any needed post-processing
        if(theEnqueueableFlagsCount > 0)
        {
		// Actually the shouldSend business is probably unnecessary now with sSmallestUnsentTick
                bool shouldSend = player_provided_flags_from_tick_to_tick(inSenderIndex, theStartTick + theRedundantActionFlagsCount, theStartTick + theRedundantActionFlagsCount + theEnqueueableFlagsCount);
                if(shouldSend && (sSmallestIncompleteTick - sSmallestUnsentTick >= sHubPreferences.mSendPeriod))
                        send_packets();
        }
} // hub_received_game_data_packet_v1()


static void
player_acknowledged_up_to_tick(size_t inPlayerIndex, int32 inSmallestUnacknowledgedTick)
{
	logTraceNMT2("player_acknowledged_up_to_tick(%d, %d)", inPlayerIndex, inSmallestUnacknowledgedTick);
	
        NetworkPlayer_hub& thePlayer = getNetworkPlayer(inPlayerIndex);

        // Ignore late ACK's
        if(inSmallestUnacknowledgedTick < thePlayer.mSmallestUnacknowledgedTick)
                return;

        // We've heard from this player
        thePlayer.mLastNetworkTickHeard = sNetworkTicker;

        // Mark us ACKed for each intermediate tick
        for(int theTick = thePlayer.mSmallestUnacknowledgedTick; theTick < inSmallestUnacknowledgedTick; theTick++)
        {
		logDumpNMT2("tick %d: sPlayerDataDisposition=%d", theTick, sPlayerDataDisposition[theTick]);
		
                assert(sPlayerDataDisposition[theTick] & (((uint32)1) << inPlayerIndex));
                sPlayerDataDisposition[theTick] &= ~(((uint32)1) << inPlayerIndex);
		if (inPlayerIndex != sLocalPlayerIndex) 
		{
			assert(theTick < sFlagSendTimeQueue.getWriteTick());
			// update the latency display
			thePlayer.mDisplayLatencyTicks -= thePlayer.mDisplayLatencyBuffer[thePlayer.mDisplayLatencyCount % thePlayer.mDisplayLatencyBuffer.size()];
			thePlayer.mDisplayLatencyBuffer[thePlayer.mDisplayLatencyCount++ % thePlayer.mDisplayLatencyBuffer.size()] = sNetworkTicker - sFlagSendTimeQueue.peek(theTick);
			thePlayer.mDisplayLatencyTicks += (sNetworkTicker - sFlagSendTimeQueue.peek(theTick));
			
		}
			
                if(sPlayerDataDisposition[theTick] == 0)
                {
                        assert(theTick == sPlayerDataDisposition.getReadTick());
			assert(theTick == sFlagSendTimeQueue.getReadTick());
			assert(theTick == sPlayerReflectedFlags.getReadTick());
                        
                        sPlayerDataDisposition.dequeue();
			sFlagSendTimeQueue.dequeue();
			sPlayerReflectedFlags.dequeue();
                        for(size_t i = 0; i < sFlagsQueues.size(); i++)
                        {
                                if(sFlagsQueues[i].size() > 0)
                                {
                                        assert(sFlagsQueues[i].getReadTick() == theTick);
                                        sFlagsQueues[i].dequeue();
                                }
                        }
                }
        }

        thePlayer.mSmallestUnacknowledgedTick = inSmallestUnacknowledgedTick;

        // If player has acknowledged timing adjustment tick, adjustment is no longer
        // outstanding, and we need to restart the data collection to get a new average.
        if(thePlayer.mOutstandingTimingAdjustment != 0 && thePlayer.mTimingAdjustmentTick < inSmallestUnacknowledgedTick)
        {
                thePlayer.mOutstandingTimingAdjustment = 0;
		thePlayer.mNthElementFinder.reset();
        }

	hub_check_for_completion();
	
} // player_acknowledged_up_to_tick()

static bool make_up_flags_for_first_incomplete_tick()
{
	// find the smallest incomplete tick, and make up flags for anybody in that tick!
	
	if (sPlayerDataDisposition.getWriteTick() == sSmallestIncompleteTick) 
		// we don't have flags for anybody!
		return false;

	// never make up flags for ourself
	if (getFlagsQueue(sLocalPlayerIndex).getWriteTick() == sSmallestIncompleteTick)
		return false;

	// check to make sure everyone we want to make up flags for is in the lagging players bitmask
	for (int i = 0; i < sNetworkPlayers.size(); i++)
	{
		if (getFlagsQueue(i).getWriteTick() == sSmallestIncompleteTick && !(sLaggingPlayersBitmask & (1 << i)))
			return false;
	}

	logTraceNMT1("making up flags for tick %i", sSmallestIncompleteTick);

	for (int i = 0; i < sNetworkPlayers.size(); i++)
	{
		if (getFlagsQueue(i).getWriteTick() == sSmallestIncompleteTick)
		{
			// network code shouldn't figure this out, someone else should
			action_flags_t motionFlags = sLastFlagsReceived[i] & (_moving | _sidestepping);
			getFlagsQueue(i).enqueue(motionFlags);
			sPlayerReflectedFlags[sSmallestIncompleteTick] |= (1 << i);
		}
	}
	sPlayerDataDisposition[sSmallestIncompleteTick] = sConnectedPlayersBitmask;
	sSmallestIncompleteTick++;
	sLastRealUpdate = sNetworkTicker;
	return true;
}

// Returns true if we now have enough data to send at least one new tick
static bool
player_provided_flags_from_tick_to_tick(size_t inPlayerIndex, int32 inFirstNewTick, int32 inSmallestUnreceivedTick)
{
	logTraceNMT3("player_provided_flags_from_tick_to_tick(%d, %d, %d)", inPlayerIndex, inFirstNewTick, inSmallestUnreceivedTick);
	
        bool shouldSend = false;

	assert(sPlayerDataDisposition.getWriteTick() == sPlayerReflectedFlags.getWriteTick());

        for(int i = sPlayerDataDisposition.getWriteTick(); i < inSmallestUnreceivedTick; i++)
        {
		logDumpNMT2("tick %d: enqueueing sPlayerDataDisposition %d", i, sConnectedPlayersBitmask);
                sPlayerDataDisposition.enqueue(sConnectedPlayersBitmask);
		sPlayerReflectedFlags.enqueue(0);
        }

        for(int i = inFirstNewTick; i < inSmallestUnreceivedTick; i++)
        {
		logDumpNMT2("tick %d: sPlayerDataDisposition=%d", i, sPlayerDataDisposition[i]);
		
                assert(sPlayerDataDisposition[i] & (((uint32)1) << inPlayerIndex));
                sPlayerDataDisposition[i] &= ~(((uint32)1) << inPlayerIndex);
		sLaggingPlayersBitmask &= ~(((uint32)1) << inPlayerIndex);
                if(sPlayerDataDisposition[i] == 0)
                {
                        assert(sSmallestIncompleteTick == i);
                        sSmallestIncompleteTick++;
			sLastRealUpdate = sNetworkTicker;
                        shouldSend = true;

                        // Now people need to ACK
                        sPlayerDataDisposition[i] = sConnectedPlayersBitmask;
                }

        } // loop over ticks with new data

        return shouldSend;

} // player_provided_flags_from_tick_to_tick()



static void
process_messages(AIStream& ps, int inSenderIndex)
{
        bool done = false;

        while(!done)
        {
                uint16	theMessageType;
                ps >> theMessageType;

                switch(theMessageType)
                {
                        case kEndOfMessagesMessageType:
                                done = true;
                                break;

                        default:
                                process_optional_message(ps, inSenderIndex, theMessageType);
                                break;
                }
        }
}



static void
process_lossy_byte_stream_message(AIStream& ps, int inSenderIndex, uint16 inLength)
{
	assert(inSenderIndex >= 0 && inSenderIndex < static_cast<int>(sNetworkPlayers.size()));

	HubLossyByteStreamChunkDescriptor theDescriptor;

	size_t theHeaderStreamPosition = ps.tellg();
	ps >> theDescriptor.mType >> theDescriptor.mDestinations;
	theDescriptor.mLength = inLength - (ps.tellg() - theHeaderStreamPosition);
	theDescriptor.mSender = inSenderIndex;

	logDumpNMT4("got %d bytes of lossy stream type %d from player %d for destinations 0x%x", theDescriptor.mLength, theDescriptor.mType, theDescriptor.mSender, theDescriptor.mDestinations);

	bool canEnqueue = true;
	
	if(sOutgoingLossyByteStreamDescriptors.getRemainingSpace() < 1)
	{
		logNoteNMT4("no descriptor space remains; discarding (%uh) bytes of lossy streaming data of distribution type %hd from player %hu destined for 0x%lx", theDescriptor.mLength, theDescriptor.mType, theDescriptor.mSender, theDescriptor.mDestinations);
		canEnqueue = false;
	}

	// We avoid enqueueing a partial chunk to make things easier on code that uses us
	if(theDescriptor.mLength > sOutgoingLossyByteStreamData.getRemainingSpace())
	{
		logNoteNMT4("insufficient buffer space for %uh bytes of lossy streaming data of distribution type %hd from player %hu destined for 0x%lx; discarded", theDescriptor.mLength, theDescriptor.mType, theDescriptor.mSender, theDescriptor.mDestinations);
		canEnqueue = false;
	}

	if(canEnqueue)
	{
		if(theDescriptor.mLength > 0)
		{
			// This is an assert, not a test, because the data buffer should be no
			// bigger than the scratch buffer (and if it didn't fit into the data buffer,
			// canEnqueue would be false).
			assert(theDescriptor.mLength <= sizeof(sScratchBuffer));

			// XXX extraneous copy, needed given the current interfaces to these things
			ps.read(sScratchBuffer, theDescriptor.mLength);
	
			sOutgoingLossyByteStreamData.enqueueBytes(sScratchBuffer, theDescriptor.mLength);
			sOutgoingLossyByteStreamDescriptors.enqueue(theDescriptor);
		}
	}
	else
		// We still have to gobble up the entire message, even if we can't use it.
		ps.ignore(theDescriptor.mLength);
}



static void
process_optional_message(AIStream& ps, int inSenderIndex, uint16 inMessageType)
{
        // All optional messages are required to give their length in the two bytes
        // immediately following their type.  (The message length value does not include
        // the space required for the message type ID or the encoded message length.)
        uint16 theMessageLength;
        ps >> theMessageLength;

	if(inMessageType == kSpokeToHubLossyByteStreamMessageType)
		process_lossy_byte_stream_message(ps, inSenderIndex, theMessageLength);
	else
	{
		// Currently we ignore (skip) all optional messages
		ps.ignore(theMessageLength);
	}
}



static void
make_player_netdead(int inPlayerIndex)
{
	logContextNMT1("making player %d netdead", inPlayerIndex);
	
        NetworkPlayer_hub& thePlayer = getNetworkPlayer(inPlayerIndex);

        thePlayer.mNetDeadTick = sSmallestIncompleteTick;
        thePlayer.mConnected = false;
        sConnectedPlayersBitmask &= ~(((uint32)1) << inPlayerIndex);
        sAddressToPlayerIndex.erase(thePlayer.mAddress);

	// We save this off because player_provided... call below may change it.
	int32 theSavedIncompleteTick = sSmallestIncompleteTick;
	
        // Pretend for housekeeping that he's provided data for all currently known ticks
        // We go from the first tick for which we don't actually have his data through the last
        // tick we actually know about.
        player_provided_flags_from_tick_to_tick(inPlayerIndex, getFlagsQueue(inPlayerIndex).getWriteTick(), sPlayerDataDisposition.getWriteTick());

        // Pretend for housekeeping that he's already acknowledged all sent ticks
        player_acknowledged_up_to_tick(inPlayerIndex, theSavedIncompleteTick);
}

static bool
hub_tick()
{
        sNetworkTicker++;

	logContextNMT1("performing hub_tick %d", sNetworkTicker);

        // Check for newly netdead players
        bool shouldSend = false;
        for(size_t i = 0; i < sNetworkPlayers.size(); i++)
        {
                int theSilentTicksBeforeNetDeath = (sNetworkPlayers[i].mSmallestUnacknowledgedTick < sSmallestRealGameTick) ? sHubPreferences.mPregameTicksBeforeNetDeath : sHubPreferences.mInGameTicksBeforeNetDeath;
                if(sNetworkPlayers[i].mConnected && sNetworkTicker - sNetworkPlayers[i].mLastNetworkTickHeard > theSilentTicksBeforeNetDeath)
                {
                        make_player_netdead(i);
                        shouldSend = true;
                }
        }
	
	// if we're getting behind, make up flags

	if (sHubPreferences.mMinimumSendPeriod >= sHubPreferences.mSendPeriod && sPlayerDataDisposition.getReadTick() >= sSmallestRealGameTick && sSmallestIncompleteTick < sPlayerDataDisposition.getWriteTick())
	{
		
		if (sNetworkTicker - sLastRealUpdate >= sHubPreferences.mMinimumSendPeriod)
		{
			// add anybody holding us back to the lagging player bitmask
			for (int i = 0; i < sNetworkPlayers.size(); i++)
			{
				if (i != sLocalPlayerIndex && sNetworkPlayers[i].mConnected && sSmallestRealGameTick > sNetworkPlayers[i].mNetDeadTick)
				{
					if (sPlayerDataDisposition[sSmallestIncompleteTick] & (1 << i))
						sLaggingPlayersBitmask |= (1 << i);
				}
			}
		}
		
		if (sLaggingPlayersBitmask) {
			// make up flags if a majority of players are ready to go
			int readyPlayers = 0;
			int nonReadyPlayers = 0;
			for (int i = 0; i < sNetworkPlayers.size(); i++)
			{
				if (sNetworkPlayers[i].mConnected && sSmallestRealGameTick > sNetworkPlayers[i].mNetDeadTick)
				{
					if (sPlayerDataDisposition[sSmallestIncompleteTick] & (1 << i))
						nonReadyPlayers++;
					else
						readyPlayers++;
				}
			}
			
			if (readyPlayers > nonReadyPlayers)
				if (make_up_flags_for_first_incomplete_tick())
					shouldSend = true;
		}
	}

#ifdef BANDWIDTH_REDUCTION
	send_packets();
#else
	
	if(shouldSend)
		send_packets();
	else
	{
		// Make sure we send at least every once in a while to keep things going
		if(sNetworkTicker > sLastNetworkTickSent && (sNetworkTicker - sLastNetworkTickSent) >= sHubPreferences.mRecoverySendPeriod)
			send_packets();
	}
	
#endif
        check_send_packet_to_spoke();

        // We want to run again.
        return true;
}



static void
send_packets()
{
	// Currently, at most one lossy data descriptor is used per trip through this function.  So,
	// we do some processing here outside the loop since the results'd be the same every time.
	HubLossyByteStreamChunkDescriptor theDescriptor;
	bool haveLossyData = false;
	if(sOutgoingLossyByteStreamDescriptors.getCountOfElements() > 0)
	{
		haveLossyData = true;
		theDescriptor = sOutgoingLossyByteStreamDescriptors.peek();

		// XXX extraneous copy due to limited interfaces
		// We assert here; the real "test" happened when it was enqueued.
		assert(theDescriptor.mLength <= sizeof(sScratchBuffer));
		sOutgoingLossyByteStreamData.peekBytes(sScratchBuffer, theDescriptor.mLength);
	}

	// remember when we sent flags for the first time
	for (int32 i = sFlagSendTimeQueue.getWriteTick(); i < sSmallestIncompleteTick; i++) 
	{
		sFlagSendTimeQueue.enqueue(sNetworkTicker);
	}
		
        for(size_t i = 0; i < sNetworkPlayers.size(); i++)
        {
                NetworkPlayer_hub& thePlayer = sNetworkPlayers[i];
                if(thePlayer.mConnected && thePlayer.mAddressKnown)
                {
			AOStreamBE hdr(sOutgoingFrame->data, kStarPacketHeaderSize);
                        AOStreamBE ps(sOutgoingFrame->data, ddpMaxData, kStarPacketHeaderSize);

                        try {
                                // acknowledgement
                                ps << getFlagsQueue(i).getWriteTick();
        
                                // Messages
                                // Timing adjustment?
                                if(thePlayer.mOutstandingTimingAdjustment != 0)
                                {
                                        ps << (uint16)kTimingAdjustmentMessageType
                                                << (int8)(thePlayer.mOutstandingTimingAdjustment);
                                }
        
                                // Netdead players?
                                for(size_t j = 0; j < sNetworkPlayers.size(); j++)
                                {
                                        if(thePlayer.mSmallestUnacknowledgedTick <= sNetworkPlayers[j].mNetDeadTick)
                                        {
                                                ps << (uint16)kPlayerNetDeadMessageType
                                                        << (uint8)j	// dead player index
                                                        << sNetworkPlayers[j].mNetDeadTick;
                                        }
                                }

				// Lossy streaming data?
				if(haveLossyData && ((theDescriptor.mDestinations & (((uint32)1) << i)) != 0))
				{
					logDumpNMT4("packet to player %d will contain %d bytes of lossy byte stream type %d from player %d", i, theDescriptor.mLength, theDescriptor.mType, theDescriptor.mSender);
					// In AStreams, sizeof(packed scalar) == sizeof(unpacked scalar)
					uint16 theMessageLength = sizeof(theDescriptor.mType) + sizeof(theDescriptor.mSender) + theDescriptor.mLength;
					
					ps << (uint16)kHubToSpokeLossyByteStreamMessageType
						<< theMessageLength
						<< theDescriptor.mType
						<< theDescriptor.mSender;

					ps.write(sScratchBuffer, theDescriptor.mLength);
				}
        
                                // End of messages
                                ps << (uint16)kEndOfMessagesMessageType;
        
                                // We use this flag to make sure we encode the start tick at most once, and
                                // only if we're actually sending action_flags.
                                bool haveSentStartTick = false;

				// are we sending an incremental update or a recovery update?
				int32 startTick;
				int32 endTick;
				// never send fewer than 2 full updates per second, or more than 15
				int effectiveLatency = std::max((int32) 2, std::min((int32) ((thePlayer.mDisplayLatencyCount > 0) ? (thePlayer.mDisplayLatencyTicks / std::min(thePlayer.mDisplayLatencyCount, (uint32) thePlayer.mDisplayLatencyBuffer.size())) : 2), (int32) (TICKS_PER_SECOND / 2)));

#ifdef BANDWIDTH_REDUCTION
				if (sNetworkTicker - thePlayer.mLastRecoverySend >= effectiveLatency)
				{
					// send a large update
					thePlayer.mLastRecoverySend = sNetworkTicker;
					
					// we want to send 4 seconds worth of flags per second
					int maxTicks = 4 * effectiveLatency;
					startTick = thePlayer.mSmallestUnacknowledgedTick;
					endTick = (startTick + maxTicks < sSmallestIncompleteTick) ? startTick + maxTicks : sSmallestIncompleteTick;
				}
				else
				{
					// send the last 3 flags
					startTick = std::max(sSmallestIncompleteTick - 3, thePlayer.mSmallestUnacknowledgedTick);
					endTick = sSmallestIncompleteTick;
				}
#else
				startTick = thePlayer.mSmallestUnacknowledgedTick;
				endTick = sSmallestIncompleteTick;
#endif

				bool reflectFlags = false;
				// find out if we need to reflect flags
				for (int32 tick = startTick; tick < endTick && !reflectFlags; tick++)
				{
					if (sPlayerReflectedFlags.peek(tick) & (1 << i)) reflectFlags = true;
				}
        
                                // Action_flags!!
                                // First, preprocess the players to figure out at what tick they'll each stop
                                // contributing
				std::vector<int32> theSmallestTickWeWontSend;
                                theSmallestTickWeWontSend.resize(sNetworkPlayers.size());
                                for(size_t j = 0; j < sNetworkPlayers.size(); j++)
                                {
                                        // Don't encode our own flags
                                        if(j == i && !reflectFlags)
                                        {
                                                theSmallestTickWeWontSend[j] = thePlayer.mSmallestUnacknowledgedTick - 1;
                                                continue;
                                        }
        
                                        theSmallestTickWeWontSend[j] = sSmallestIncompleteTick;
                                        NetworkPlayer_hub& theOtherPlayer = sNetworkPlayers[j];
        
                                        // Don't send flags for netdead people
                                        if(!theOtherPlayer.mConnected && theSmallestTickWeWontSend[j] > theOtherPlayer.mNetDeadTick)
                                                theSmallestTickWeWontSend[j] = theOtherPlayer.mNetDeadTick;
                                }
        
                                // Now, encode the flags in tick-major order (this is much easier to decode
                                // at the other end)
                                for(int32 tick = startTick; tick < endTick; tick++)
                                {
                                        for(size_t j = 0; j < sNetworkPlayers.size(); j++)
                                        {
                                                if(tick < theSmallestTickWeWontSend[j])
                                                {
                                                        if(!haveSentStartTick)
                                                        {
                                                                ps << tick;
                                                                haveSentStartTick = true;
                                                        }
                                                        ps << getFlagsQueue(j).peek(tick);
                                                }
                                        }
                                }
				
				hdr << (uint16) (reflectFlags ? kHubToSpokeGameDataPacketWithSpokeFlagsV1Magic : kHubToSpokeGameDataPacketV1Magic);
				hdr << calculate_data_crc_ccitt(&sOutgoingFrame->data[kStarPacketHeaderSize], ps.tellp() - kStarPacketHeaderSize);
        
                                // Send the packet
                                sOutgoingFrame->data_size = ps.tellp();
                                if(i == sLocalPlayerIndex)
                                        send_frame_to_local_spoke(sOutgoingFrame, &thePlayer.mAddress, kPROTOCOL_TYPE, 0 /* ignored */);
                                else
                                        NetDDPSendFrame(sOutgoingFrame, &thePlayer.mAddress, kPROTOCOL_TYPE, 0 /* ignored */);
                        } // try
                        catch (...)
                        {
				logWarningNMT("Caught exception while constructing/sending outgoing packet");
                        }
                        
                } // if(connected)

        } // iterate over players

        sLastNetworkTickSent = sNetworkTicker;
	sSmallestUnsentTick = sSmallestIncompleteTick;

	if(haveLossyData)
	{
		sOutgoingLossyByteStreamData.dequeue(theDescriptor.mLength);
		sOutgoingLossyByteStreamDescriptors.dequeue();
	}
	
} // send_packets()


int32 hub_latency(int player_index)
{
	if (player_index != sLocalPlayerIndex)
	{
		NetworkPlayer_hub& thePlayer = sNetworkPlayers[player_index];
		if (!thePlayer.mConnected) return kNetLatencyDisconnected;
		if (thePlayer.mDisplayLatencyCount >= thePlayer.mDisplayLatencyBuffer.size())
		{
			int32 latency_ticks = std::max(thePlayer.mDisplayLatencyTicks, (sNetworkTicker - thePlayer.mLastNetworkTickHeard) * (int32) thePlayer.mDisplayLatencyBuffer.size());
			return (latency_ticks * 1000 / TICKS_PER_SECOND / thePlayer.mDisplayLatencyBuffer.size());
		} 
	}

	return kNetLatencyInvalid;
}

static inline const char *BoolString(bool B) {return (B ? "true" : "false");}

enum {
	// kOutgoingFlagsQueueSizeAttribute,
	kPregameTicksBeforeNetDeathAttribute,
	kInGameTicksBeforeNetDeathAttribute,
	kPregameWindowSizeAttribute,
	kInGameWindowSizeAttribute,
	kPregameNthElementAttribute,
	kInGameNthElementAttribute,
	kSendPeriodAttribute,
	kRecoverySendPeriodAttribute,
	kMinimumSendPeriodAttribute,
	kNumAttributes
};

static const char* sAttributeStrings[kNumAttributes] =
{
	//	"outgoing_flags_queue_size",
	"pregame_ticks_before_net_death",
	"ingame_ticks_before_net_death",
	"pregame_window_size",
	"ingame_window_size",
	"pregame_nth_element",
	"ingame_nth_element",
	"send_period",
	"recovery_send_period",
	"minimum_send_period",
};

static int32* sAttributeDestinations[kNumAttributes] =
{
	//	&sHubPreferences.mOutgoingFlagsQueueSize,
	&sHubPreferences.mPregameTicksBeforeNetDeath,
	&sHubPreferences.mInGameTicksBeforeNetDeath,
	&sHubPreferences.mPregameWindowSize,
	&sHubPreferences.mInGameWindowSize,
	&sHubPreferences.mPregameNthElement,
	&sHubPreferences.mInGameNthElement,
	&sHubPreferences.mSendPeriod,
	&sHubPreferences.mRecoverySendPeriod,
	&sHubPreferences.mMinimumSendPeriod,
};

static const int32 sDefaultHubPreferences[kNumAttributes] = {
	kDefaultPregameTicksBeforeNetDeath,
	kDefaultInGameTicksBeforeNetDeath,
	kDefaultPregameWindowSize,
	kDefaultInGameWindowSize,
	kDefaultPregameNthElement,
	kDefaultInGameNthElement,
	kDefaultSendPeriod,
	kDefaultRecoverySendPeriod,
	kDefaultMinimumSendPeriod,
};

class XML_HubConfigurationParser: public XML_ElementParser
{
public:
	bool Start();
	bool HandleAttribute(const char *Tag, const char *Value);
	bool AttributesDone();

	XML_HubConfigurationParser(): XML_ElementParser("hub") {}

protected:

        bool	mAttributePresent[kNumAttributes];
	int32	mAttribute[kNumAttributes];
};

bool XML_HubConfigurationParser::Start()
{
        for(int i = 0; i < kNumAttributes; i++)
                mAttributePresent[i] = false;

	return true;
}

static const char* sAttributeMultiplySpecifiedString = "attribute multiply specified";

bool XML_HubConfigurationParser::HandleAttribute(const char *Tag, const char *Value)
{
	for(size_t i = 0; i < kNumAttributes; i++)
	{
		if(StringsEqual(Tag,sAttributeStrings[i]))
		{
			if(!mAttributePresent[i]) {
				if(ReadInt32Value(Value,mAttribute[i])) {
					mAttributePresent[i] = true;
					return true;
				}
				else
					return false;
			}
			else {
				ErrorString = sAttributeMultiplySpecifiedString;
				return false;
			}
		}
	}
	
	UnrecognizedTag();
	return false;
}

bool XML_HubConfigurationParser::AttributesDone() {
	// Ignore out-of-range values
	for(int i = 0; i < kNumAttributes; i++)
	{
		if(mAttributePresent[i])
		{
			switch(i)
			{
			case kPregameTicksBeforeNetDeathAttribute:
			case kInGameTicksBeforeNetDeathAttribute:
			case kRecoverySendPeriodAttribute:
			case kSendPeriodAttribute:
			case kPregameWindowSizeAttribute:
			case kInGameWindowSizeAttribute:
				if(mAttribute[i] < 1)
				{
					// I don't know whether this actually does anything if I don't return false,
					// but I'd like to honor the user's wishes as far as I can without just throwing
					// up my hands.
					BadNumericalValue();
					logWarning3("improper value %d for attribute %s of <hub>; must be at least 1.  using default of %d", mAttribute[i], sAttributeStrings[i], *(sAttributeDestinations[i]));
					mAttributePresent[i] = false;
				}
				else
				{
					*(sAttributeDestinations[i]) = mAttribute[i];
				}
				break;
				
			case kPregameNthElementAttribute:
			case kInGameNthElementAttribute:
				if(mAttribute[i] < 0)
				{
					BadNumericalValue();
					logWarning3("improper value %d for attribute %s of <hub>; must be at least 1.  using default of %d", mAttribute[i], sAttributeStrings[i], *(sAttributeDestinations[i]));
					mAttributePresent[i] = false;
				}
				else
				{
					*(sAttributeDestinations[i]) = mAttribute[i];
				}
				break;
				
			case kMinimumSendPeriodAttribute:
				if (mAttribute[i] < 0)
				{
					BadNumericalValue();
					logWarning3("improper value %d for attribute %s of <hub>; must be at least 0. using default of %d", mAttribute[i], sAttributeStrings[i], *(sAttributeDestinations[i]));
					mAttributePresent[i] = false;
				} 
				else
				{
					*(sAttributeDestinations[i]) = mAttribute[i];
				}
				break;
				
				
			default:
				assert(false);
				break;
			} // switch(attribute)
			
		} // if attribute present
		
	} // loop over attributes
	
	// The checks above are not sufficient to catch all bad cases; if user specified a window size
	// smaller than default, this is our only chance to deal with it.
	if(sHubPreferences.mPregameNthElement >= sHubPreferences.mPregameWindowSize) {
		logWarning5("value for <hub> attribute %s (%d) must be less than value for %s (%d).  using %d", sAttributeStrings[kPregameNthElementAttribute], sHubPreferences.mPregameNthElement, sAttributeStrings[kPregameWindowSizeAttribute], sHubPreferences.mPregameWindowSize, sHubPreferences.mPregameWindowSize - 1);
		
		sHubPreferences.mPregameNthElement = sHubPreferences.mPregameWindowSize - 1;
	}
	
	if(sHubPreferences.mInGameNthElement >= sHubPreferences.mInGameWindowSize) {
		logWarning5("value for <hub> attribute %s (%d) must be less than value for %s (%d).  using %d", sAttributeStrings[kInGameNthElementAttribute], sHubPreferences.mInGameNthElement, sAttributeStrings[kInGameWindowSizeAttribute], sHubPreferences.mInGameWindowSize, sHubPreferences.mInGameWindowSize - 1);
		
		sHubPreferences.mInGameNthElement = sHubPreferences.mInGameWindowSize - 1;
	}
	
	return true;
}


static XML_HubConfigurationParser HubConfigurationParser;


XML_ElementParser*
Hub_GetParser() {
	return &HubConfigurationParser;
}



void
WriteHubPreferences(FILE* F)
{
	fprintf(F, "    <hub\n");
	for(size_t i = 0; i < kNumAttributes; i++)
	{
		if(*(sAttributeDestinations[i]) != sDefaultHubPreferences[i])
			fprintf(F, "      %s=\"%d\"\n", sAttributeStrings[i], *(sAttributeDestinations[i]));
	}
	fprintf(F, "    />\n");

	fprintf(F, "    <!-- current hub defaults:\n");
	fprintf(F, "      DO NOT EDIT THE FOLLOWING - they're FYI only.  Make settings in 'hub' tag above.\n");
	for(size_t i = 0; i < kNumAttributes; i++)
		fprintf(F, "      %s=\"%d\"\n", sAttributeStrings[i], sDefaultHubPreferences[i]);
	fprintf(F, "    -->\n");
}



void
DefaultHubPreferences()
{
	for(size_t i = 0; i < kNumAttributes; i++)
		*(sAttributeDestinations[i]) = sDefaultHubPreferences[i];
/*
	sHubPreferences.mPregameWindowSize = kDefaultPregameWindowSize;
	sHubPreferences.mInGameWindowSize = kDefaultInGameWindowSize;
	sHubPreferences.mPregameNthElement = kDefaultPregameNthElement;
	sHubPreferences.mInGameNthElement = kDefaultInGameNthElement;
	sHubPreferences.mPregameTicksBeforeNetDeath = kDefaultPregameTicksBeforeNetDeath;
	sHubPreferences.mInGameTicksBeforeNetDeath = kDefaultInGameTicksBeforeNetDeath;
	sHubPreferences.mSendPeriod = kDefaultSendPeriod;
	sHubPreferences.mRecoverySendPeriod = kDefaultRecoverySendPeriod;
*/
}

#endif // !defined(DISABLE_NETWORKING)
