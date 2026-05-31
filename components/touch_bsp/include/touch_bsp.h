#ifndef TOUCH_BSP_H
#define TOUCH_BSP_H

#ifdef __cplusplus
extern "C" {
#endif

void Touch_Init(void);
uint8_t getTouch(uint16_t *x,uint16_t *y);

// Put the FT3168 into hibernate (lowest power). Touch is not polled during
// streaming, so this is called when entering streaming mode.
void Touch_Sleep(void);

#ifdef __cplusplus
}
#endif

#endif
