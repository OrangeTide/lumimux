/* lu_umask.h : restrictive umask for runtime directory files */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef LU_UMASK_H
#define LU_UMASK_H

/* Set umask(077) and save the original value. Call once at startup. */
void lu_umask_save(void);

/* Restore the original umask saved by lu_umask_save(). Call in child
 * processes before exec'ing a user shell. */
void lu_umask_restore(void);

#endif
