#ifndef WINE_DSR_CONTROL_H
#define WINE_DSR_CONTROL_H

#include <stdint.h>

/* Private SystemParametersInfoW protocol. Fields use fixed widths. */
#define WINE_SPI_DSR_CONTROL 0x44535201u
#define WINE_DSR_CONTROL_VERSION 1u
#define WINE_DSR_PHYSICAL_MODE ((uint32_t)-3)

enum wine_dsr_method
{
    WINE_DSR_AUTO = 0,
    WINE_DSR_OFF = 1,
    WINE_DSR_GAUSSIAN = 2,
    WINE_DSR_NEURAL = 3
};

enum wine_dsr_operation
{
    WINE_DSR_QUERY = 0,
    WINE_DSR_SET = 1
};

enum wine_dsr_capability
{
    WINE_DSR_CAP_CONTROL = 1,
    WINE_DSR_CAP_GAUSSIAN = 2,
    WINE_DSR_CAP_NEURAL = 4
};

enum wine_dsr_reason
{
    WINE_DSR_READY = 0,
    WINE_DSR_WAITING = 1,
    WINE_DSR_ASSETS = 2,
    WINE_DSR_DEVICE = 3,
    WINE_DSR_FORMAT = 4,
    WINE_DSR_FACTOR = 5,
    WINE_DSR_SETUP = 6,
    WINE_DSR_NATIVE = 7,
    WINE_DSR_DISABLED = 8,
    WINE_DSR_PRESENTATION = 9
};

struct wine_dsr_control
{
    uint64_t hwnd;
    uint32_t size;
    uint32_t version;
    uint32_t operation;
    uint32_t method;
    uint32_t smoothness;
    uint32_t capabilities;
    uint32_t active_method;
    uint32_t active_smoothness;
    uint32_t reason;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t generation;
    uint32_t applied_generation;
    int32_t result;
};

#endif
