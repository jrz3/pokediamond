#ifndef GUARD_SND_COMMAND_SHARED_H
#define GUARD_SND_COMMAND_SHARED_H

#include "nitro/types.h"

#define SND_CMD_COUNT 256
#define SND_CMD_ARG_MAX 4

#define SND_CMD_FLAG_NOBLOCK    0x0
#define SND_CMD_FLAG_BLOCK      0x1
#define SND_CMD_FLAG_IMMEDIATE  0x2

#define SND_CMD_START_SEQ               0x00
#define SND_CMD_STOP_SEQ                0x01
#define SND_CMD_PREPARE_SEQ             0x02
#define SND_CMD_START_PREPARED_SEQ      0x03
#define SND_CMD_PAUSE_SEQ               0x04
#define SND_CMD_SKIP_SEQ                0x05
#define SND_CMD_PLAYER_PARAM            0x06
#define SND_CMD_TRACK_PARAM             0x07
#define SND_CMD_MUTE_TRACK              0x08
#define SND_CMD_ALLOCATABLE_CHANNEL     0x09
#define SND_CMD_PLAYER_LOCAL_VAR        0x0A
#define SND_CMD_PLAYER_GLOBAL_VAR       0x0B
#define SND_CMD_START_TIMER             0x0C
#define SND_CMD_STOP_TIMER              0x0D
#define SND_CMD_SETUP_CHANNEL_PCM       0x0E
#define SND_CMD_SETUP_CHANNEL_PSG       0x0F
#define SND_CMD_SETUP_CHANNEL_NOISE     0x10
#define SND_CMD_SETUP_CAPTURE           0x11
#define SND_CMD_SETUP_ALARM             0x12
#define SND_CMD_CHANNEL_TIMER           0x13
#define SND_CMD_CHANNEL_VOLUME          0x14
#define SND_CMD_CHANNEL_PAN             0x15
#define SND_CMD_SURROUND_DECAY          0x16
#define SND_CMD_MASTER_VOLUME           0x17
#define SND_CMD_MASTER_PAN              0x18
#define SND_CMD_OUTPUT_SELECTOR         0x19
#define SND_CMD_LOCK_CHANNEL            0x1A
#define SND_CMD_UNLOCK_CHANNEL          0x1B
#define SND_CMD_STOP_UNLOCKED_CHANNEL   0x1C
#define SND_CMD_SET_SHARED_WORK         0x1D
#define SND_CMD_INVALIDATE_SEQ          0x1E
#define SND_CMD_INVALIDATE_BANK         0x1F
#define SND_CMD_INVALIDATE_WAVE         0x20
#define SND_CMD_READ_DRIVER_INFO        0x21

struct SNDCommand {
    struct SNDCommand *llNext;  // 0x0
    s32 id;                     // 0x4
    u32 arg[SND_CMD_ARG_MAX];   // 0x8
};  // size = 0x18

#endif //GUARD_SND_COMMAND_SHARED_H
