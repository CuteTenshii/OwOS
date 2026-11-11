/* pwrap.c: PMIC register access over the WACS2 channel of the MT6765 PMIC wrapper. */
#include "pwrap.h"
#include "mmio.h"
#include "spinlock.h"

#define INIT_DONE2 0xa0
#define WACS2_CMD 0xc20
#define WACS2_RDATA 0xc24
#define WACS2_VLDCLR 0xc28

#define RDATA_VALUE(v) ((v) & 0xffff)
#define RDATA_FSM(v) (((v) >> 16) & 7)
#define RDATA_INIT_DONE (1u << 21)
#define CMD_WRITE (1u << 31)
#define FSM_IDLE 0
#define FSM_WFVLDCLR 6

#define POLL_LIMIT 100000

#define DEW_READ_TEST_MT6357 0x040c
#define DEW_READ_TEST_MT6358 0x040e
#define DEW_READ_TEST_VAL 0x5aa5

static uintptr_t base;
static spinlock_t lock;

static int wait_fsm(uint32_t state)
{
  for (int i = 0; i < POLL_LIMIT; i++)
    if (RDATA_FSM(mmio_read32(base + WACS2_RDATA)) == state)
      return 0;
  return -1;
}

/* A read whose result was never collected leaves the FSM waiting for VLDCLR; release it for the next access. */
static int wait_idle(void)
{
  if (!wait_fsm(FSM_IDLE))
    return 0;
  if (RDATA_FSM(mmio_read32(base + WACS2_RDATA)) == FSM_WFVLDCLR)
    mmio_write32(base + WACS2_VLDCLR, 1);
  return -1;
}

int pwrap_init(uintptr_t b)
{
  base = 0;
  if (!mmio_read32(b + INIT_DONE2) || !(mmio_read32(b + WACS2_RDATA) & RDATA_INIT_DONE))
    return -1;
  base = b;
  return 0;
}

int pmic_read(uint16_t reg, uint16_t *val)
{
  if (!base || (reg & 1))
    return -1;
  uintptr_t f = spin_lock_irqsave(&lock);
  int rc = wait_idle();
  if (!rc) {
    mmio_write32(base + WACS2_CMD, (uint32_t)(reg >> 1) << 16);
    rc = wait_fsm(FSM_WFVLDCLR);
  }
  if (!rc) {
    *val = (uint16_t)RDATA_VALUE(mmio_read32(base + WACS2_RDATA));
    mmio_write32(base + WACS2_VLDCLR, 1);
  }
  spin_unlock_irqrestore(&lock, f);
  return rc;
}

int pmic_write(uint16_t reg, uint16_t val)
{
  if (!base || (reg & 1))
    return -1;
  uintptr_t f = spin_lock_irqsave(&lock);
  int rc = wait_idle();
  if (!rc)
    mmio_write32(base + WACS2_CMD, CMD_WRITE | (uint32_t)(reg >> 1) << 16 | val);
  spin_unlock_irqrestore(&lock, f);
  return rc;
}

const char *pmic_probe(void)
{
  uint16_t v;
  if (!pmic_read(DEW_READ_TEST_MT6357, &v) && v == DEW_READ_TEST_VAL)
    return "mt6357";
  if (!pmic_read(DEW_READ_TEST_MT6358, &v) && v == DEW_READ_TEST_VAL)
    return "mt6358";
  return 0;
}
