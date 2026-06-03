/*
 * TaterTOS64v3 — <linux/kdev_t.h>
 *
 * Device major/minor definitions.
 */

#ifndef _TATERTOS_LINUX_KDEV_T_H
#define _TATERTOS_LINUX_KDEV_T_H

#define MINORBITS       20
#define MINORMASK       ((1U << MINORBITS) - 1)

#define MAJOR(dev)      ((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)      ((unsigned int) ((dev) & MINORMASK))
#define MKDEV(ma,mi)    (((ma) << MINORBITS) | (mi))

#endif
