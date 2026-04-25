#ifndef GS_USB_H
#define GS_USB_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* USB vendor request codes */
#define GS_USB_BREQ_BITTIMING       0x01
#define GS_USB_BREQ_MODE            0x02
#define GS_USB_BREQ_DEVICE_CONFIG   0x05

/* gs_device_mode.mode */
#define GS_CAN_MODE_RESET  0
#define GS_CAN_MODE_START  1

/* gs_host_frame.echo_id value that marks a received (not echoed TX) frame */
#define GS_CAN_EVENT_RX_FRAME  0xFFFFFFFFU

/* CAN ID flags (same as Linux socketcan) */
#define CAN_EFF_FLAG  0x80000000U
#define CAN_RTR_FLAG  0x40000000U
#define CAN_ERR_FLAG  0x20000000U
#define CAN_SFF_MASK  0x000007FFU
#define CAN_EFF_MASK  0x1FFFFFFFU

#pragma pack(push, 1)

struct gs_device_bittiming {
    u32 prop_seg;
    u32 phase_seg1;
    u32 phase_seg2;
    u32 sjw;
    u32 brp;
};

struct gs_device_mode {
    u32 mode;
    u32 feature;
};

struct gs_host_frame {
    u32 echo_id;
    u32 can_id;
    u8  can_dlc;
    u8  channel;
    u8  flags;
    u8  reserved;
    u8  data[8];
};

#pragma pack(pop)

/* Supported bitrates for STM32 CAN peripheral at 48 MHz: 16 TQ/bit */
#define BITTIMING_125K  { .prop_seg=1, .phase_seg1=11, .phase_seg2=3, .sjw=1, .brp=24 }
#define BITTIMING_250K  { .prop_seg=1, .phase_seg1=11, .phase_seg2=3, .sjw=1, .brp=12 }
#define BITTIMING_500K  { .prop_seg=1, .phase_seg1=11, .phase_seg2=3, .sjw=1, .brp=6  }
#define BITTIMING_1000K { .prop_seg=1, .phase_seg1=11, .phase_seg2=3, .sjw=1, .brp=3  }

#endif /* GS_USB_H */
