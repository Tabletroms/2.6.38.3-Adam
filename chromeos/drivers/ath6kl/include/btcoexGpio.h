#define GPIO_A      (15)
#define GPIO_B      (16)
#define GPIO_C      (17)
#define GPIO_D      (18)
#define GPIO_E      (19)
#define GPIO_F      (21)
#define GPIO_G      (21)





#define GPIO_DEBUG_WORD_1                                 (1<<GPIO_A)
#define GPIO_DEBUG_WORD_2                   (1<<GPIO_B)
#define GPIO_DEBUG_WORD_3                  ((1<<GPIO_B) | (1<<GPIO_A))
#define GPIO_DEBUG_WORD_4     (1<<GPIO_C)
#define GPIO_DEBUG_WORD_5    ((1<<GPIO_C) |               (1<<GPIO_A))
#define GPIO_DEBUG_WORD_6    ((1<<GPIO_C) | (1<<GPIO_B))
#define GPIO_DEBUG_WORD_7    ((1<<GPIO_C) | (1<<GPIO_B) | (1<<GPIO_A))

#define GPIO_DEBUG_WORD_8     (1<<GPIO_D)
#define GPIO_DEBUG_WORD_9    ((1<<GPIO_D) | GPIO_DEBUG_WORD_1)
#define GPIO_DEBUG_WORD_10   ((1<<GPIO_D) | GPIO_DEBUG_WORD_2)
#define GPIO_DEBUG_WORD_11   ((1<<GPIO_D) | GPIO_DEBUG_WORD_3)
#define GPIO_DEBUG_WORD_12   ((1<<GPIO_D) | GPIO_DEBUG_WORD_4)
#define GPIO_DEBUG_WORD_13   ((1<<GPIO_D) | GPIO_DEBUG_WORD_5)
#define GPIO_DEBUG_WORD_14   ((1<<GPIO_D) | GPIO_DEBUG_WORD_6)
#define GPIO_DEBUG_WORD_15   ((1<<GPIO_D) | GPIO_DEBUG_WORD_7)

#define GPIO_DEBUG_WORD_16   (1<<GPIO_E)
#define GPIO_DEBUG_WORD_17    ((1<<GPIO_E) | GPIO_DEBUG_WORD_1)
#define GPIO_DEBUG_WORD_18   ((1<<GPIO_E) | GPIO_DEBUG_WORD_2)
#define GPIO_DEBUG_WORD_19   ((1<<GPIO_E) | GPIO_DEBUG_WORD_3)
#define GPIO_DEBUG_WORD_20   ((1<<GPIO_E) | GPIO_DEBUG_WORD_4)
#define GPIO_DEBUG_WORD_21   ((1<<GPIO_E) | GPIO_DEBUG_WORD_5)
#define GPIO_DEBUG_WORD_22   ((1<<GPIO_E) | GPIO_DEBUG_WORD_6)
#define GPIO_DEBUG_WORD_23   ((1<<GPIO_E) | GPIO_DEBUG_WORD_7)



extern void btcoexDbgPulseWord(A_UINT32 gpioPinMask);
extern void btcoexDbgPulse(A_UINT32 pin);
