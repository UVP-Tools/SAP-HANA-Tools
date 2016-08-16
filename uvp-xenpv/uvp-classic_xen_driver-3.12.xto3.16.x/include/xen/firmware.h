#ifndef __XEN_FIRMWARE_H__
#define __XEN_FIRMWARE_H__

#if IS_ENABLED(CONFIG_EDD)
void copy_edd(void);
#endif

#ifdef CONFIG_XEN_PRIVILEGED_GUEST
void copy_edid(void);
#else
static inline void copy_edid(void) {}
#endif

#endif /* __XEN_FIRMWARE_H__ */
