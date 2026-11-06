#include "browser.h"
#include "mutex.h"

static mutex_t blk;

void browser_buf_lock(void) { mutex_lock(&blk); }

void browser_buf_unlock(void) { mutex_unlock(&blk); }
